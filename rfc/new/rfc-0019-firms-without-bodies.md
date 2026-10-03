# RFC 0019: Firms Without Bodies {#rfc_firms_without_bodies}

Status: proposal

## Problem

Every way into a firm takes a callable body: `with_firm(fn)`,
`with_firm<Policy>(fn)`, `deck.sync_wait(fn)`, `rt.run(fn)`. The body exists
for one reason: a task's frame is allocated from the current firm when the
task is created (RFC 0002), so a firm can only hold its body's frames if the
body is created after the firm is current. The callable delays creation.

That one mechanism grew into the shape of the API:

- `firm_body<Policy, Fn>` inherits from the policy firm and stores the
  callable, so the scope is also the body.
- `run_firm_body` must handle a body that returns with unjoined children:
  stop them, join, and report. This is a hand-built `async with`.
- `with_firm(lambda)` keeps the closure alive, `scope.fork(fn, args)` does
  not. Identical-looking capturing coroutine lambdas have different lifetime
  rules.
- Root entry builds two firms: `sync_wait`'s root firm, then `with_firm`'s.
- Each call site instantiates `run_firm`, `run_firm_body` and
  `with_env_bound` for its own lambda type. A `sync_wait` call site costs
  about 107 ms of compile time against 11 ms for a plain coroutine; the test
  suite has hundreds.

Meanwhile the code that needs concurrency does not need bodies. Outside the
tests, every use is one of:

| Shape | Uses |
| --- | --- |
| root arena and stop scope, no children | `rt.run`, `nxtrt::run`, `run_with_kqueue`, `sync_wait`, nxtllm and openai-sse request firms |
| fixed set of ideas plus a stop rule | `when_all` (cgroup), `with_timeout`, `poll_until`, tool_process capture + monitor, shell_scope's four loops |
| range of ideas | `when_all_range` (fs), `wait_any_range` (DNS racing) |
| feed of ideas, bounded | HTTP server connections, AI tool batches, echo bench accept loop |

The fixed and range shapes are hand-written fork/fork/join bodies or tuple
lowering wrapped in a policy firm. Since 3eac756 the tuple path already runs
an ordinary pool inside `with_firm<Policy>(lambda)`; the policy firm only
supplies the stop rule.

## Proposal

Split the three jobs of a firm and drop the body.

1. **Frames.** A firm is frame land plus a stop source. The deck's root entry
   owns the root firm and binds it while it invokes the root factory, once.
   Groups allocate their jobs' frames from the ambient firm, as pools do now.
   No API creates a nested firm by running a body.

2. **Work ownership.** Concurrent work is always a group of ideas: a tuple, a
   range or a feed, admitted into a bounded pool. A group has a stop rule and
   settles every job before it returns. There is no `fork`, `join`, `deed` or
   `catching_deed` in the public API, and no child records.

3. **Results.** A group yields outcomes: a typed tuple for a fixed set, a
   completion-order feed for a pool. Helpers turn outcomes into the usual
   `when_all` and `wait_any` results.

Because a group is an awaited operation, not a scope with a body, there is no
"body returned with unjoined children" state to recover from. Unbounded
fork/join can return later as a library on top of pools, if a use appears
that pools cannot express.

## API sketch

Names are provisional.

```cpp
// Root: one firm per entry, factory invoked once with it bound.
auto n = deck.sync_wait(count_lines, path);          // fn, args...
rt.run(serve, options);

// Fixed set: settle all, stop rule chosen per call.
auto [a, b] = co_await nxtrt::when_all(load_a, load_b);      // stop on failure
auto first  = co_await nxtrt::wait_any(via_v4, via_v6);      // stop on success
auto outcomes = co_await nxtrt::settle(                       // no stop rule
    std::tuple{read_pty, pump_stdin, sample_cgroup, render});

// Primary plus companions: companions stop when the primary settles.
auto result = co_await nxtrt::supervise(
    capture_output(state), std::tuple{monitor(state)});

// Range of ideas.
auto entries = co_await nxtrt::when_all_range(stat_ideas);

// Feed of ideas, bounded: land lives in the caller's frame.
auto connections = nxtrt::bounded_pool<connection_idea, 64>{accepted};
co_await nxtrt::drain(connections);   // consume to EOF, close on failure
```

`bounded_pool<Idea, N>` owns its farm, slots and output cells, so the common
case is one declaration instead of the four borrowed pieces `pool` takes
today. `pool<Idea>` stays for callers that lend their own land.

Ideas are any callables returning `task<T>` or `hope<T>`. Plain functions,
`std::bind_front` and small structs (like `connection_recipe`) are preferred
to capturing coroutine lambdas; a capturing lambda that only *returns* a task
from a named coroutine is fine.

## The two examples

**shell_scope_demo** forks four loops into a lambda body, polls
`state.process_done`, then joins and inspects four `catching_deed`s. As a
group, the loops are the ideas and the outcomes replace the deeds:

```cpp
auto [reader, input, sampler, renderer] = co_await nxtrt::settle(std::tuple{
    std::bind_front(read_pty_until_done, std::ref(pty), std::ref(state)),
    std::bind_front(pump_stdin_to_pty, std::ref(pty), std::ref(state)),
    std::bind_front(sample_cgroup_until_done, std::ref(state)),
    std::bind_front(render_until_done, std::ref(terminal), std::ref(state),
                    std::ref(pty)),
});
```

The polling loop disappears: each loop already ends when the process does.

**Echo bench** forks a server, N clients and a timeout into one scope; the
server forks one task per accepted connection. As groups, the accept loop is
a feed of connection ideas into a bounded pool, the way the HTTP server
already works, and the load is a supervised set:

```cpp
task<void> echo_server(int listener, std::size_t clients, ...)
{
    auto accepted = accept_feed{listener, clients};       // feed<echo_idea>
    auto connections = nxtrt::bounded_pool<echo_idea, 64>{accepted};
    co_await nxtrt::drain(connections);
}

co_await nxtrt::supervise(
    wait_until_done(state),                              // primary
    std::tuple{
        std::bind_front(echo_server, listener, clients, ...),
        client_ideas(options, address, payload, stats, state),  // range
        std::bind_front(timeout_load, options.timeout, state),
    });
```

The connection bound becomes explicit: today the bench forks every client
connection without one.

## Test runner

Tests can be coroutines. The runner owns a deck and enters it once per test
through one non-template path:

```cpp
"evacuate results"_test = []() -> nxtrt::task<void> {
    auto v = co_await value_after_yield(7);
    expect(v == 7_i);
};
```

Tests that need their own deck configuration keep calling `sync_wait`.

## What goes away

- `with_firm`, `firm_body`, `run_firm`, `run_firm_body`, `operator co_await`
  on firms, `sync_wait_firm`.
- `firm::fork`, `firm::join`, `deed`, `catching_deed`, child records,
  `completed(task_id, exception_ptr)` as a firm virtual (a stop rule becomes
  a group parameter), and the firm subclasses `stop_on_failure`,
  `stop_on_success`, `stop_on_completion` as firms.
- Most of the runtime suite's `firms` group, which tests fork/join/deed
  semantics. Its frame-land and cancellation cases move to groups.

`nxtrt/runtime.rkt` loses `spawned`, deed observation and child records in
favour of pool slots, which it already models.

## Open questions

- **Nested frame land.** Nothing outside tests borrows bounded frame land or
  needs a nested arena. If one is needed, an RAII guard that rebinds the
  current task's firm for frames created in its extent would provide it
  without a body. Its destructor must find no live frames in that land.
- **Games.** `with_game` and `sync_wait_game` bind a game scope around a body
  in the same way. They should become root-entry options or a bound value.
- **Cancellation of a group from outside.** A group's jobs observe the
  awaiting task's stop token; a stop rule stops the group's pool. Is an
  explicit `stop()` handle on a running group ever needed?
- **Recipe lifetimes.** `bounded_pool` keeps recipes alive through execution,
  as `pool` does. Fixed groups keep the tuple in the awaiting frame. Ranges
  move each idea into a slot before invoking it.
