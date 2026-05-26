# Runtime consolidation notes

The application/runtime surface has moved from the old `nxtio` stack onto
`nxt::rt`. `libcoro` has been removed from the Meson build and from
`subprojects`, and the remaining `nxtio` sources have been deleted. The old
custom task prototype is gone; its useful ideas now belong in `nxt::rt::env`,
task zones, and explicit UI/runtime capabilities.

## Current Shape

`src` owns the coroutine substrate:

- `nxt::rt::task<T>` for lazy coroutine tasks.
- `nxt::rt::deck` for pumpable execution.
- `nxt::rt::wand` implementations for platform waiting.
- `nxt::rt::with_zone`, `fork`, `deed`, `when_all`, and timeout helpers.
- DNS, HTTP, TLS, and socket experiments.
- Linux subprocess wishes for piped children, pty children, pidfd waits, and
  pidfd signals.
- OpenAI Responses request JSON, streaming, and basic tool-call batches.
- Core terminal input types and parsing in `src/nxtui/input.hpp`.
- The default `nxtllm` executable, including one-shot streaming and
  `read_file`/`rg_search`/`bash` tool execution on `nxt::rt`.

The old application stack is no longer in the tree. The former `src/nxt/ai`
LLM stack has also been removed; the surviving LLM code lives in
`src/nxt/llm`.

## Migration table

| Old surface | New target | Notes |
| --- | --- | --- |
| `nxt::task<T>` | `nxt::rt::task<T>` | Start with leaf code that does not expose scheduler handles. |
| `nxt::scheduler` | `nxt::rt::deck` + `nxt::rt::wand` | Keep the host pumpable so terminal and Emacs embeddings can own the event loop. |
| `scheduler.yield_for(d)` | `nxt::rt::op::timeout::after(d)` or `with_timeout` | Keep sleep/yield as runtime methods at the app boundary. |
| `scheduler.poll(...)` | `nxt::rt::op::poll*` | Convert call sites once they are inside an `nxt::rt::task`. |
| `nxt::queue<T>` | `nxt::rt::channel<T>` | Done. Used for UI input, resize, and tool streams. |
| `nxt::event` | `nxt::rt::event` | Done. Used for damage notifications and small UI coordination points. |
| `nxtio/input.hpp` | `nxt/input.hpp` | Done. The compatibility include has been removed. |
| `nxt::latch` | zone join/deeds or a small latch | Prefer structured joins; add a latch only for true countdown cases. |
| `spawn_detached` | `nxt::rt::fork` in a zone | Detached work should still be owned by a root zone. |
| `nxt::scope` | `nxt::rt::task_zone` + UI capabilities | The runtime side is split out; the richer yard-style UI facade is still being rebuilt on top. |
| `nxtio/net` | `src` HTTP/TLS/DNS | Done. The OpenAI streaming path uses the new HTTP client directly. |
| old shell/pty subprocess helpers | `nxt::rt::op::spawn_pty` + `nxt::rt::pty::session` | PTY processes are now pidfd-owned wishes and can render through vterm without a separate output mailbox. |
| old LLM entry point | `src/nxt/llm/nxtllm.cpp` | Done. The executable streams one-shot turns and runs tool batches; richer interactive HUD work remains. |

## Completed Slices

1. Add `nxt::rt` channel and event primitives. Done as deck-local
   `nxt::rt::channel<T>` and manual-reset `nxt::rt::event`.

2. Introduce a runtime facade beside terminal UI helpers. Done as
   `nxt::rt::runtime`: it owns a `deck`, platform `wand`, root-zone run
   entrypoint, damage event, input channel, resize channel, and `sleep`.
   `nxt::rt::terminal_app` and the runtime demos layer terminal/compositor
   ownership on top of it.

3. Port buffer and HTTP helpers to `nxt::rt::task`. Done in `src/nxt/rt`.
   Keep request/response data structures free of runtime dependencies.
   `src/nxt/llm/responses_request.hpp` is the model for that split.

4. Make OpenAI streaming use `src` networking. Done for the one-shot
   `nxtllm` path: it connects over `nxt::rt` TCP/TLS, reads HTTP/SSE, and
   collects completed output items.

5. Port tool execution after streaming works. Done as
   `src/nxt/llm/tool_batch.hpp`, `agent_tools.hpp`, and
   `tool_process.hpp`. Batches fork one task per call in a zone and return
   ordered `function_call_output` items.

6. Re-enable `nxtllm` on the runtime. Done in
   `src/nxt/llm/nxtllm.cpp`: the executable builds by default, parses CLI
   options, enters `nxt::rt::runtime`, streams responses over the `src`
   HTTP/TLS stack, and can complete a bash tool-call smoke test.

7. Use PTYs for live tool surfaces. Started with `nxt-shell-scope-demo`: the
   command runs through `spawn_pty`, feeds a `vterm` session, and renders as a
   TUI surface. Cgroup sampling reads files through `openat`/`read_some`
   wishes and batches each sample with `when_all`.

## Compatibility strategy

Do not restore a compatibility alias layer. The old libcoro task and
`nxt::rt::task` have different ownership and pump semantics, and a dual alias
layer would hide the hard parts. Prefer explicit ports:

- Move dependency-light data types first.
- Port leaf async functions next.
- Keep old UI demos only when they teach an unported subsystem; delete them
  once the runtime version covers the same behavior.
- Treat a clean default Meson build as the guardrail for runtime-only work.

## Remaining Work

The core migration is done. The remaining work is product shape:

1. Build the richer interactive `nxtllm` HUD on top of the runtime UI pieces.
2. Decide which demos still carry their weight after the runtime consolidation.
3. Keep request construction and JSON parsing runtime-neutral.
4. Keep the default Meson build as the guardrail for runtime-only work.
