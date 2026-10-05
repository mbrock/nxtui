# NXTAI: current pieces and next steps {#ai_overview}

`nxtllm` is a tool-using OpenAI Responses client running on the ordinary NXT
deck and wand. It defaults to `gpt-6-luna` and advertises `read_file`,
`rg_search`, and `bash`, using the current directory as its working directory.

```sh
nix develop .#filc -c build/filc/nxt-dev nxtllm "Does this repo support Fil-C?"
build/nxtllm --no-tools "Explain epoll briefly"
build/src/nxtllm --websocket "Inspect the failing tests and suggest a fix"
build/nxtllm --dump-request "hello from nxtrt"
```

Set `OPENAI_API_KEY` first. Development shells configure the TLS trust bundle.
Tools run with the caller's filesystem and process permissions; `--no-tools`
disables them. `bash` and `rg` must be on PATH (the C++ development shells
provide both). `--max-turns N` bounds model requests, defaulting to 32.

## Ownership and response handling

`responses_transport.hpp` owns each connection, verified TLS session, HTTP
decoding reader, and SSE feed. The CLI and native hosts share this transport;
each call borrows the transport and an observer with awaitable
`text(std::string)`, and owns a copied request and its network buffers.
`responses_stream.hpp` decodes events independently of terminal
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

The default HTTP `store=false` mode replays complete output items, preserving opaque
reasoning content and unknown fields. Requests include
`reasoning.encrypted_content`. With `--store`, continuation uses
`previous_response_id` plus the tool results. Tool definitions are sent on
every request. Transcript ownership is independent of call parsing.

## WebSocket alternative

`--websocket` selects `responses_websocket_transport.hpp` instead of HTTP/SSE.
It authenticates the Upgrade to `wss://api.openai.com/v1/responses`, then sends
`response.create` JSON events and decodes the same Responses streaming events.
The transport owns one persistent, verified TLS/WebSocket connection across
turns. `run_agent` sends the previous response ID and **only new tool results**,
even with `store=false`: OpenAI keeps continuation state in a connection-local
memory cache. It does not require enabling server-side response storage.
`--websocket --dump-request` prints the event envelope without credentials.

Native callers can use the same transport without the CLI:

```cpp
#include <nxtai/responses_websocket_transport.hpp>

auto transport = nxtai::responses_websocket_transport{{.ca_file = public_ca_path}};
auto request = nxtai::responses::openai_responses_request{
    .api_key = runtime_key, .model = "gpt-6-luna", .input = prompt,
};
auto first = co_await transport(request, observer);
request.previous_response_id = first.id;
request.input = "Explain that in more detail."; // Only the new input.
auto second = co_await transport(request, observer);
```

Keep the transport and observer alive through their tasks. The transport is
immovable, confined to one deck, and rejects overlapping turns or changed
credentials. Omitting `previous_response_id` starts a new chain on the same
socket. This implementation uses one default lane, not multiplexed streams.

Errors, premature closure, observer exceptions, and cancellation discard the
connection after draining I/O; no automatic retry or reconnection can duplicate
generation. Construct a new transport to recover. For `store=false`, the old
socket's cache is gone: omit the ID and replay the full context (retain output
items, including encrypted reasoning, if recovery is needed). With `store=true`,
a persisted ID can continue on a new connection. OpenAI currently limits
connections to 60 minutes; cache misses surface as `previous_response_not_found`.
The agent loop propagates these failures rather than automatically replaying.

The [OpenAI WebSocket guide](https://developers.openai.com/api/docs/guides/websocket-mode)
reports up to roughly 40% faster end-to-end execution for workflows with 20+
tool calls. That is OpenAI's reported result, not a benchmark of this client.
The local WSS fixture independently checks authentication, same-connection
incremental tool continuation, fresh chains, event errors, cancellation/drain,
and header injection rejection, without external services or real credentials.

The console observer prints model text on stdout and tool names/status on
stderr. `tool_tui.hpp` and `trace_tui.hpp` remain available for richer observers;
the old UI runtime is not part of the agent loop.

Offline fixtures in `test/ai-agent-test.cpp` exercise the model/tool/model
loop, both history modes, reasoning preservation, multiple calls, unknown-tool
results, cancellation, turn limits, malformed events, and premature EOF.

## Native Windows/UWP transport

The installed `nxtrt-iocp` package exports the same HTTP/TLS/Responses and
agent headers without linking POSIX process/terminal code, Wisp, or libvterm:

```cpp
#include <nxtai/responses_transport.hpp>

// Inside a task running on an IOCP deck. Keep transport and observer alive
// through completion or cancellation drain. observer.text is awaitable.
auto transport = nxtai::responses_transport{{.ca_file = public_ca_path}};
auto request = nxtai::responses::openai_responses_request{
    .api_key = runtime_key,
    .model = "gpt-6-luna",
    .input = prompt,
};
auto response = co_await transport(std::move(request), observer);
```

`run_agent` accepts this transport and a host-defined registry. The supplied
filesystem/shell tools remain POSIX-only; a conversation needs no registry.
The caller owns conversational history between calls (append output items
verbatim, including opaque reasoning content, then the next user item).
Request `reasoning.encrypted_content` for stateless reasoning continuation.

UWP must supply an explicit public PEM CA bundle from its package or
LocalState. Chain, server-purpose, expiry, key-usage and SAN hostname/IP
verification remain mandatory. No native system-store fallback or verification
switch exists. The bundle is read with the app CRT into libcrypto's memory
BIO; the UWP library itself has no stdio/config/environment loading. Update
public roots as part of packaging; the application must not trust certificates
received from the server merely because they arrived over the network.

Load credentials at runtime from a separately provisioned LocalState file;
never bake them into source, a derivation, Nix store path, package or logs.
The transport neither loads nor logs credentials and has no retries or model
substitution. Hosts provide timeouts/cancellation using ordinary task groups.
See [building](building.md) for the portable probe and cross-build commands.

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
