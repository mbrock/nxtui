# NXTAI: current pieces and next steps {#ai_overview}

NXTAI is a collection of useful LLM building blocks, not yet an integrated
tool-using agent. The executable `nxtllm` sends one OpenAI Responses request
and prints a text stream. Tool execution is a separate library path, currently
exercised by tests rather than that executable.

## What is connected today

`src/nxtai/nxtllm.cpp` owns the connection, TLS session, decoded HTTP body,
SSE feed, and text consumer. They run on the ordinary NXT deck and wand;
there are no LLM workers or separate scheduler. TLS authenticates the peer
against the requested host and trust store. The CLI can also serialize a
request without contacting OpenAI:

```sh
build/nxtllm --dump-request "hello from nxtrt"
```

The current text consumer expects a particular event sequence: response
creation, progress, one output item, one content part, text deltas, and
completion. It does not yet dispatch arbitrary/interleaved output items,
reasoning events, function calls, or all terminal/error states. It is a useful
transport experiment, not a general Responses stream implementation.

`responses_request.hpp` already serializes tool definitions, raw input items,
and `previous_response_id`. `tool_batch.hpp` parses completed function-call
items and builds corresponding `function_call_output` items. Those pieces are
not yet connected into subsequent model requests. The older HTTP request
helper also returns `nxt::http::request`, while the CLI separately constructs
an `nxtrt::http::request`; that duplication is a future consolidation target.

The tool-turn renderer and trace-rendering sources are not built into
`nxtllm`. They should not be mistaken for an active tool UI.

## Tools as pool work

A `function_call_idea` owns one call and borrows a stable `tool_registry`.
It produces a `task<function_call_result>` and can go directly into an ordinary
`pool<function_call_idea>`:

```diagram
┌──────────────┐     ┌──────────────┐     ┌─────────────────┐
│ Tool recipes │────▶│ Bounded pool │────▶│ Result feed     │
└──────────────┘     └──────────────┘     │ completion order│
                                        └────────┬────────┘
                                                 ▼
                                         Result consumer
```

Each result retains its call and call ID, tool result, and serialized output
item. There is no need to keep a deed or spawn into an ambient firm. The firm
still supplies frame/cancellation context; the pool owns execution and drain.
The registry, input feed, slots, output land, and any referenced state must
survive that drain. Use `finally(consume(pool), close_factory)` for consumers
that may fail or stop before EOF.

For callers that want a complete ordered vector, the convenience API is:

```cpp
auto results = co_await nxtai::tools::run_function_tool_batch(
    registry, std::move(calls), 2); // at most two admitted jobs
```

Its default capacity is four; zero is invalid. It creates recipes lazily,
reuses bounded slots, and writes outcomes to their original input positions.
It does not allocate firm child records or retain a vector of deeds. Calls
may finish out of order, but returned results remain in input order.
The batch still retains all calls and outputs: bounded admission is not an
aggregate memory budget or a guarantee that concurrent tools are independent.

### Failure and cancellation

- Unknown tools, invalid parsed arguments, and ordinary exceptions from typed
  tools become per-call failed results. They do not stop unrelated calls.
- Uncaught registry/infrastructure exceptions are collected by the batch;
  all calls settle before the first input-order failure is rethrown. The
  direct pool follows its normal stop/drain/rethrow behavior instead.
- Cancellation is control flow, not a failed tool-output string. It stops
  admission, cancels running work, and awaits settlement before propagating.
  Pool cleanup is shielded from cancellation; settlement remains cooperative.

These contracts are covered by deterministic tests for asymmetric completion,
slot reuse, input ordering, typed errors, infrastructure failures, and stop
at each startup turn and during running work. A separate test consumes tool
ideas directly as a completion-order pool feed.

## What the existing tools provide

`agent_tools.hpp` defines `read_file`, `rg_search`, and `bash`, and constructs
a registry through `for_agent()`. No production caller currently installs
that registry into an LLM request.

Process capture already has useful ownership: combined stdout/stderr,
bounded captured output (8 MiB by default), child wait/termination cleanup,
and optional systemd/cgroup observation. Capture plus monitoring uses tuple
composition, which already lowers its main work to pool jobs. This small
composite resource operation need not become another long-lived worker pool.

Important limitations remain:

- The bash tool's read-only guidance is descriptive, not enforced permission
  or sandboxing. File paths are not restricted to a workspace.
- `read_file` uses synchronous filesystem operations and may stall the single
  event loop. Its size cap silently truncates; some read failures yield empty
  text. `rg_search` treats a nonzero exit, including no matches, as failure.
- Individual output caps do not bound all retained batch output, and there is
  no tool-batch deadline or dependency/side-effect ordering policy.
- Process-capture tests exist, but there is no end-to-end model/tool/model
  fixture. The request stream consumer itself lacks offline event-sequence
  coverage.

## Recommended next work

1. Extract a reusable Responses event decoder with offline fixtures covering
   multiple/interleaved items, reasoning, function arguments, failure,
   incomplete responses, and premature EOF. Keep terminal formatting separate.
2. Connect one response → completed tool calls → bounded tool pool → output
   items → next response. Test the entire loop with a scripted local server
   before relying on live provider calls. Make conversation ownership explicit.
3. Consume completion-order tool results for progress reporting, collecting an
   ordered turn only where needed. Then reconnect the UI to real lifecycle
   events rather than another collection of worker/deed bookkeeping.
4. Establish tool permissions, filesystem scope, deadlines, and aggregate
   output budgets before exposing shell execution as a general agent feature.

This keeps one scheduler and existing task/feed/pool composition, rather than
adding an agent-specific concurrency framework.
