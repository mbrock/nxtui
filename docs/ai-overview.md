# NXTAI: current pieces and next steps {#ai_overview}

`nxtllm` is a tool-using OpenAI Responses client running on the ordinary NXT
deck and wand. It defaults to `gpt-6-luna` and advertises `read_file`,
`rg_search`, and `bash`, using the current directory as its working directory.

```sh
nix develop .#filc -c build/filc/nxt-dev nxtllm "Does this repo support Fil-C?"
build/nxtllm --no-tools "Explain epoll briefly"
build/nxtllm --dump-request "hello from nxtrt"
```

Set `OPENAI_API_KEY` first. Development shells configure the TLS trust bundle.
Tools run with the caller's filesystem and process permissions; `--no-tools`
disables them. `bash` and `rg` must be on PATH (the C++ development shells
provide both). `--max-turns N` bounds model requests, defaulting to 32.

## Ownership and response handling

`nxtllm.cpp` owns each connection, verified TLS session, HTTP decoding reader,
and SSE feed. `responses_stream.hpp` decodes events independently of terminal
rendering. Text and refusal deltas stream immediately; argument and reasoning
updates do not trigger tool execution. A successful `response.completed`
snapshot supplies the canonical ordered output items, so interleaved events
cannot reorder conversation history. Failed/incomplete responses and premature
EOF fail the turn without executing its partial calls.

`agent.hpp` owns the request and local transcript. After each completed
response, it extracts function calls, invokes the existing tool pool, appends
`function_call_output` items, and requests the next response. No calls means
the turn is finished. The CLI uses pool capacity one to preserve tool order;
library callers can explicitly choose more concurrency. Cancellation propagates
through the pool's existing stop/drain behavior. The final allowed model turn
cannot start tools whose results would require another request.

The default `store=false` mode replays complete output items, preserving opaque
reasoning content and unknown fields. Requests include
`reasoning.encrypted_content`. With `--store`, continuation uses
`previous_response_id` plus the tool results. Tool definitions are sent on
every request. Transcript ownership is independent of call parsing.

The console observer prints model text on stdout and tool names/status on
stderr. `tool_tui.hpp` and `trace_tui.hpp` remain available for richer observers;
the old UI runtime is not part of the agent loop.

Offline fixtures in `test/ai-agent-test.cpp` exercise the model/tool/model
loop, both history modes, reasoning preservation, multiple calls, unknown-tool
results, cancellation, turn limits, malformed events, and premature EOF.

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
item. There is no need to keep per-call handles or spawn into an ambient scope.
Cancellation comes from the consuming task's stop; the pool owns execution and
drain.
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
It does not allocate per-call child records or retain a vector of handles.
Calls may finish out of order, but returned results remain in input order.
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
a registry through `for_agent()`. The CLI installs that registry by default.

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
- Individual output caps do not bound the aggregate retained transcript, and
  there is no per-tool deadline. Sequential execution preserves call order but
  does not make shell commands read-only.

## Next work

Add aggregate context/output budgets and configurable tool deadlines. Richer
progress rendering can consume tool results in completion order through the
existing pool feed, while keeping history in model output order. Filesystem
scope and tool permissions need explicit policy before this becomes a sandboxed
agent. The current CLI is a local tool runner with the user's permissions.
