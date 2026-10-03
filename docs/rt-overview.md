# nxtrt: a tour of the runtime {#rt_overview}

`nxtrt` is a single-threaded, structured coroutine runtime for C++23. Programs
are written as coroutines returning `task<T>`; a scheduler called the *deck*
resumes them; I/O is requested as platform-neutral *wishes* and performed by a
backend called the *wand* (%io_uring, epoll, or kqueue). On top of that sit
structured concurrency (groups and bounded pools), buffered value and byte
streams, and the file, HTTP, TLS, and subprocess helpers.

This page walks through those pieces in the order you meet them when writing a
program. The class and function pages are the exact reference; @ref rt_holding
is the essay version, about why the pieces have the shapes they have.

```text
protocols    fs · http · http::serve · tls · subprocess · pty · terminal_app
streams      feed<T> / sink<T> · bytefeed / bytesink · hope<T>
concurrency  settle · when_all · wait_any · with_timeout · pool<Idea> · blocking_pool
execution    task<T> · deck
backend      op::* wishes → urge → wand (uring | epoll | kqueue)
```

[TOC]

## A first program {#rt_first}

```cpp
#include <nxtrt.hpp>
#include <iostream>
using namespace std::chrono_literals;

nxtrt::task<int> slow_answer()
{
    co_await nxtrt::op::timeout::after(20ms);
    co_return 42;
}

nxtrt::task<void> main_task()
{
    std::cout << co_await slow_answer() << '\n';               // 42

    auto [a, b] = co_await nxtrt::when_all(slow_answer(), slow_answer());
    std::cout << a + b << '\n';                                 // 84, after ~20ms

    try {
        co_await nxtrt::with_timeout(5ms, slow_answer());
    } catch (nxtrt::timeout_error const &) {
        std::cout << "timed out\n";
    }
}

int main()
{
    auto rt = nxtrt::runtime{};
    rt.run(main_task);
}
```

@ref nxtrt::runtime "runtime" owns one deck and the platform's default wand.
`run` takes a task *factory*, not a task: it calls `main_task()` inside the
runtime so that the root task captures the runtime's environment, then drives
the deck and wand until that task finishes and returns its result (or
rethrows its exception). Extra arguments are forwarded to the factory, as in
`rt.run(count_lines, fd)`.

## Tasks {#rt_task}

@ref nxtrt::task "task<T>" is the coroutine return type and an owning handle
to the coroutine frame. Three facts carry most of the model:

- **Tasks are lazy.** Calling a coroutine creates its frame and stops at the
  first instruction. Nothing runs until a deck resumes it.
- **Tasks own their frame.** `task` is move-only; destroying it destroys the
  frame. Awaiting a task does not transfer ownership.
- **Control always goes through the deck.** `co_await child` makes the
  awaiting task the child's *continuation* and enqueues the child. When the
  child finishes, it enqueues its continuation for a later deck round instead
  of resuming it inline.

The awaited result is returned by value, and an exception that escapes the
child is rethrown in the awaiting task.

A task also carries a stop state (see [cancellation](#rt_cancel)), a
@ref nxtrt::task_id "task_id" assigned by the deck, and a copy of the
*runtime environment*: a small set of ambient, task-local bindings (the
current deck, trace span, and keys bound with
@ref nxtrt::with_env "with_env") inherited by the tasks it awaits.

### hope: maybe already here {#rt_hope}

Many operations usually complete without waiting. A read from a stream whose
buffer already holds the bytes needs no coroutine frame and no deck round.
@ref nxtrt::hope "hope<T>" expresses that: it holds either a ready `T` or a
pending `task<T>`. Awaiting a ready hope never suspends; awaiting a pending
one splices the task in exactly as `co_await task` would. Functions with a
fast path return `hope<T>`, so the fast path costs a function call.

```cpp
nxtrt::hope<int> cached_or_fetch(int key)
{
    if (auto hit = cache.find(key); hit != cache.end())
        return nxtrt::hope<int>::ready(hit->second);
    return fetch(key);          // a task<int>, run only if awaited
}
```

### Observing completion {#rt_completion}

`task.on_completed(fn)` registers a synchronous, `noexcept` callback that runs
when the task reaches final suspension, without starting the task or taking
its result. It returns a @ref nxtrt::completion_link "completion_link"
that owns the callable and must stay alive until notification; destroying it
detaches. A callback runs inside the completing task's final suspension, so
it must not destroy that task or resume another coroutine inline. Queue
continuations instead. Groups and pools are built on these links.

## The deck {#rt_deck}

@ref nxtrt::deck "deck" is the scheduler: a FIFO queue of ready task ids and
a fixed-capacity registry that maps ids to live coroutine frames. It owns no
thread and never blocks.

- `run_ready()` runs **one round**: it takes the tasks that are ready now,
  resumes each, and then calls the attached wand's `wave()`. Tasks made ready
  during the round wait for the next round. Calling `run_ready()` from inside
  a running task throws: the deck is not reentrant.
- `run_until_idle()` repeats rounds until nothing is ready.
- `sync_wait(factory, args...)` creates a root task inside a root environment
  and pumps until it completes. If the root is suspended and nothing is ready,
  no wand event can wake it, so `sync_wait` throws a deadlock error with a
  dump of the parked tasks.
- `yield()` puts the current task at the back of the queue.

A default deck registers up to 4096 live tasks; pass a
@ref nxtrt::static_deck_task_storage "static_deck_task_storage<N>" to choose
the capacity and avoid the allocation. Because the deck only runs when its
host calls it, it can be embedded in another event loop (a terminal UI, an
editor module) that pumps it when convenient.

## Wishes, urges, and wands {#rt_wish}

Task code never calls `read(2)` or `io_uring_enter`. It awaits a *wish*: a
small value in @ref nxtrt::op "nxtrt::op" that names an operation and its
arguments.

| Wish | Result |
| --- | --- |
| `op::read_some`, `op::write_some`, `op::recv_some`, `op::send_some` | bytes transferred |
| `op::connect`, `op::accept` | connected socket / accepted fd |
| `op::poll`, `op::poll_until` | ready events (and whether it timed out) |
| `op::timeout` | nothing; completes at a deadline |
| `op::openat`, `op::openat2`, `op::statx`, `op::getdents64` | file descriptors and metadata |
| `op::spawn_piped`, `op::spawn_pty`, `op::wait_child`, `op::signal_child` | child processes |

Awaiting a wish:

1. asks the active wand to **prepare** it. The wand stages backend state and
   returns an @ref nxtrt::urge "urge<T>", keyed by a *coin* (a number naming
   this pending operation);
2. the urge always suspends, **parking** the task in the wand as a
   @ref nxtrt::need "need" (a coroutine handle plus its promise);
3. after the deck round, `wave()` **submits** everything staged during the
   round, as one batch where the backend allows it;
4. when the platform completes the operation, the wand stores the result in
   the urge and **puts the task back on the deck**.

The wish says *what*; the wand decides *how*. The same task code runs on
%io_uring, on epoll (with blocking work moved to threads where needed), and on
kqueue on macOS and the BSDs. The default wand is chosen at configure time with
`-Ddefault_wand=auto|uring|epoll|kqueue`; `auto` means io_uring on Linux,
kqueue on macOS and the BSDs, and epoll under Fil-C. See @ref building.

### Wands {#rt_wand}

@ref nxtrt::wand "wand" is the abstract backend: `prep` stages a wish and
returns its coin, `suspend` parks a need on a coin, `cancel` requests
cancellation, and `wave` flushes staged work and harvests completions. The
concrete backends are `uring_wand`, `epoll_wand`, and `kqueue_wand`.

Each pending operation moves through a fixed lifecycle: *prepared* (staged by
`prep`), *parked* (a task waits on it), *settled* (the task has been
resumed), and *retired* (its backend state is gone). A cancelled operation
resumes its task with `operation_cancelled`, but its backend state retires
only once the kernel can no longer write into it; for %io_uring that means
after the cancellation's own completion arrives. This lifecycle is specified
in the executable model [`nxtrt/runtime.rkt`](#rt_model), and
`detail::wand_exec::lifecycle` is its C++ form.

An io_uring operation that already completed still delivers its result,
including accepted/opened descriptors and read bytes; the exec stays alive
until any outstanding cancellation CQE drains. For readiness-driven child
waits, cancellation before `waitid` leaves the child unreaped for cleanup.

The readiness backends preserve caller descriptor flags between syscalls and
honor the requested accept flags. Socket send/receive use `MSG_DONTWAIT`;
generic read/write, connect, and accept temporarily enable `O_NONBLOCK` and
restore it immediately. That temporary change is visible to other threads
using the same open-file description (including `dup` aliases), so concurrent
flag changes or blocking I/O on those aliases require caller synchronization.

## Cancellation {#rt_cancel}

Cancellation in nxtrt is a stop request on a task, and it is cooperative.

- **Stop flows downward along awaits.** When a task is awaited, it follows
  the awaiting task's stop state, so stopping a task stops whatever it is
  currently awaiting.
- **Parked wishes are cancelled.** A task stopped while waiting on a wish
  asks the wand to cancel that operation; if cancellation wins, the await
  throws @ref nxtrt::operation_cancelled "operation_cancelled". An operation
  that already completed still returns its result. A task already stopped
  when it awaits a wish has it cancelled immediately.
- **Groups stop their own jobs.** A [group](#rt_group) stops its remaining
  jobs when its policy says so or when the group itself is stopped, and
  waits for all of them to finish before returning.
- **Code checks explicitly elsewhere.** Pure computation sees a stop only if
  it calls `throw_if_stop_requested()`, `stop_requested()`, or
  `current_stop_token()`.

`task.request_stop()` stops a task you own. @ref nxtrt::shield "shield(task)"
awaits a task *without* passing stop to it, which is how
@ref nxtrt::finally "finally" runs cleanup even when the body was cancelled.

## Groups: a fixed set of tasks {#rt_group}

A group runs several tasks concurrently and does not return until every one it
started has finished. Nothing outlives the expression that awaits it.

```cpp
auto [page, icon] = co_await nxtrt::settle(
    std::tuple{fetch(page_url), fetch(icon_url)});
if (!page)
    std::rethrow_exception(page.error());
```

@ref nxtrt::settle "settle" takes a tuple of tasks and returns one
@ref nxtrt::outcome "outcome<T>" per task, in input order. An outcome is
`std::expected<T, std::exception_ptr>`; a task stopped before it started
settles as cancelled. @ref nxtrt::settle_range "settle_range" does the same
for a range of tasks of one type and returns a vector.

The second argument is a *policy*, called as each task finishes, that decides
whether to stop the rest:

| Policy | Stops the others when |
| --- | --- |
| `all_group` (default) | never |
| `fail_fast_group` | a task fails |
| `first_success_group` | a task succeeds |
| `first_completion_group` | any task finishes |
| `primary_group` | task 0 finishes (the rest are companions) |

A policy is any `bool(std::size_t index, bool failed) noexcept` callable, so
a lambda works too. Stopping is a request: the group still waits for every
stopped task to finish before returning. A stop chosen by the policy is a
normal return; a stop from outside cancels the group's tasks, drains them,
and then throws `operation_cancelled`.

The everyday combinators are written over `settle`:

- @ref nxtrt::when_all "when_all(tasks...)" and `when_all_range` return all
  values (with `std::monostate` for `void`). On a failure they stop the rest
  and, once those have drained, rethrow the failure that triggered the stop
  (the first to complete, whatever its position), never the
  `operation_cancelled` of a task it stopped; use `settle` to see every
  failure;
- @ref nxtrt::wait_any "wait_any(tasks...)" and `wait_any_range` return the
  first success *in completion order*, and throw all failures together if nothing
  succeeds;
- @ref nxtrt::with_timeout "with_timeout(duration, task)" races a task
  against a timer and throws `timeout_error` if the timer wins;
- @ref nxtrt::poll_until_after "poll_until_after(fd, events, timeout)" races
  a readiness poll against a timer.

Groups accept tasks, not factories: create the tasks before entering the group
and keep any state they borrow alive yourself.

## Pools: a stream of work {#rt_pool_overview}

When the work is not a fixed set but an open-ended stream, use a
@ref nxtrt::pool "pool". A pool reads *ideas* from a feed, runs at most
`capacity` of them at once, and publishes their results as another feed, in
completion order. An @ref nxtrt::idea "idea" is a movable callable that
returns `task<T>` or `hope<T>`; the pool invokes each one only after
admitting it, so unadmitted work costs nothing.

A finished job keeps its slot until its result is consumed, so a slow
consumer slows admission: the bound covers buffered results as well as
running work. When you don't need the results,
@ref nxtrt::drain "drain(ideas, capacity)" runs everything and rethrows the
first failure after stopping and draining the rest.

The full pool contract, with an example, is in @ref rt_pool.

## Blocking work {#rt_blocking_overview}

A synchronous C++ call that may block (a slow library, CPU-heavy work) must not
run on the deck. @ref nxtrt::blocking_pool "blocking_pool" runs such
callables on a fixed set of worker threads; the awaiting task stays on its
deck and resumes there with the result or exception. Only owned callables and
results cross threads. See @ref rt_blocking.

## Streams {#rt_io}

@ref nxtrt::feed "feed<T>" and @ref nxtrt::sink "sink<T>" are buffered
streams of values, modelled on Zig's `std.Io.Reader` and `Writer`. A stream
owns its buffer. Reading verbs (`take`, `peek`, `take_until`, `stream`, ...)
return a `hope`: when the buffer already has what was asked for, the result is
ready and no coroutine is involved; only a refill calls the stream's virtual
"get more" edge and suspends. A stack of streams (socket → TLS → HTTP body →
server-sent events) therefore reads buffered data without any suspensions.

@ref nxtrt::bytefeed "bytefeed" and @ref nxtrt::bytesink "bytesink" are the
byte versions, with concrete sources and sinks for file descriptors, sockets,
and memory.

```cpp
nxtrt::task<std::size_t> count_lines(int fd)
{
    auto input = nxtrt::fd_source{fd};
    auto lines = std::size_t{0};
    try {
        for (;;) {
            co_await input.take_until("\n");
            ++lines;
        }
    } catch (nxtrt::end_of_stream const &) {
    }
    co_return lines;
}
```

A view returned by `take_until` or `peek` points into the stream's buffer and
is valid until the next read from that stream.

## Protocols and processes {#rt_protocols}

These are libraries written with the pieces above, not scheduler primitives:

- @ref nxtrt::fs "nxtrt::fs": files, directory listings, and confined opens
  beneath a directory handle.
- @ref nxtrt::http "nxtrt::http": an HTTP/1.1 client over byte streams, with
  chunked and compressed bodies, and `http::serve`, a bounded HTTP/1.1
  server.
- @ref nxtrt::tls "nxtrt::tls": a TLS 1.3 client that verifies certificates
  with libcrypto.
- @ref nxtrt::subprocess "nxtrt::subprocess" and @ref nxtrt::pty "nxtrt::pty":
  child processes with pipes or a pseudo-terminal, using pidfds on Linux and
  `EVFILT_PROC` on kqueue.
- `nxtrt::terminal_app`: owns a terminal session for a UI program: raw
  input mode, the terminal size, and an `nxtui` compositor to draw with.

## The formal model {#rt_model}

The runtime's lifecycle rules are also written down as an executable model,
`nxtrt/runtime.rkt` in the repository. It is written in `#lang rdf-forge`, a
small language that is at once an ontology of the runtime's concepts and a
bounded temporal model checker. It describes decks, pools, tasks, wishes, and
the lifecycle of wand executions, states invariants such as "an execution
retires only when no kernel event can still arrive for it", and searches
bounded traces for counterexamples. `make spec` checks it; see @ref building.

## Rules of thumb {#rt_pitfalls}

- **Never let a capturing coroutine lambda's task outlive the lambda.** A
  coroutine frame does not copy the closure object it was called on. If
  `[&]() -> task<void> { ... }` is a temporary, its captures dangle as soon as
  the full-expression ends. Prefer named coroutine functions with explicit
  parameters. A non-coroutine lambda that *returns* a task
  (`[&] { return work(x); }`) is fine, and `run`/`sync_wait` keep their
  factory alive for the root task's lifetime.
- **Pass factories to entry points, tasks to groups.** `rt.run(main_task)`,
  not `rt.run(main_task())`; `settle(std::tuple{a(), b()})`, not factories.
- **Don't block the deck.** A blocking syscall or a long computation stalls
  every task. Use wishes for I/O and a `blocking_pool` for the rest.
- **Keep borrowed storage outside the consuming coroutine.** Pools, feeds, and
  their storage are often borrowed. Declare them where they outlive both the
  consumer and the cleanup that closes them.
- **One deck, one thread.** Decks, tasks, and streams are confined to the
  thread that runs their deck. `blocking_pool` is the supported way across.
- **A task that seems stuck on a timer usually isn't.** First suspect a
  lifetime bug or a completion that can never be signalled. Send the process
  `SIGUSR1` (installed by `runtime`) for a dump that lists each parked task
  and the wish it is waiting on.
