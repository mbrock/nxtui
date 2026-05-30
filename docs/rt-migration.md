# Runtime consolidation notes

The application/runtime surface has moved from the old `nxtio` stack onto
`nxtrt`. `libcoro` has been removed from the Meson build and from
`subprojects`, and the remaining `nxtio` sources have been deleted. The old
custom task prototype is gone; its useful ideas now belong in `nxtrt::env`,
firms, and explicit UI/runtime capabilities.

## Current Shape

`src` owns the coroutine substrate:

- `nxtrt::task<T>` for lazy coroutine tasks.
- `nxtrt::deck` for pumpable execution.
- `nxtrt::wand` implementations for platform waiting.
- `nxtrt::with_firm`, `fork`, `deed`, `when_all`, and timeout helpers.
- DNS, HTTP, TLS, and socket experiments.
- Linux subprocess wishes for piped children, pty children, pidfd waits, and
  pidfd signals.
- OpenAI Responses request JSON and small SSE streaming clients.
- Core terminal input types and parsing in `src/nxtui/input.hpp`.
- The default `nxtllm` executable as a minimal one-shot streaming client.

The old application stack is no longer in the tree. The former `src/nxt/ai`
LLM stack has also been removed; the surviving LLM code lives in
`src/nxtai`.

## Migration table

| Old surface | New target | Notes |
| --- | --- | --- |
| `nxt::task<T>` | `nxtrt::task<T>` | Start with leaf code that does not expose scheduler handles. |
| `nxt::scheduler` | `nxtrt::deck` + `nxtrt::wand` | Keep the host pumpable so terminal and Emacs embeddings can own the event loop. |
| `scheduler.yield_for(d)` | `nxtrt::op::timeout::after(d)` or `with_timeout` | Keep sleep/yield as runtime methods at the app boundary. |
| `scheduler.poll(...)` | `nxtrt::op::poll*` | Convert call sites once they are inside an `nxtrt::task`. |
| `nxt::queue<T>` | `nxtrt::wire<T>` | Done. Used for UI input, resize, and tool streams. |
| `nxt::event` | `nxtrt::bell` | Done. Used for damage notifications and small UI coordination points. |
| `nxtio/input.hpp` | `nxtui/input.hpp` | Done. The compatibility include has been removed. |
| `nxt::latch` | firm join/deeds or a small latch | Prefer structured joins; add a latch only for true countdown cases. |
| `spawn_detached` | `nxtrt::fork` in a firm | Detached work should still be owned by a root firm. |
| `nxt::scope` | `nxtrt::firm` + UI capabilities | The runtime side is split out; the richer yard-style UI facade is still being rebuilt on top. |
| `nxtio/net` | `src` HTTP/TLS/DNS | Done. The OpenAI streaming path uses the new HTTP client directly. |
| old shell/pty subprocess helpers | `nxtrt::op::spawn_pty` + `nxtrt::pty::session` | PTY processes are now pidfd-owned wishes and can render through vterm without a separate output mailbox. |
| old LLM entry point | `src/nxtai/nxtllm.cpp` | Simplified. The executable is now a small one-shot SSE client without the old HUD/tool UI runtime path. |

## Completed Slices

1. Add `nxtrt` wire and bell primitives. Done as bounded
   `nxtrt::wire<T>` and manual-reset `nxtrt::bell`.

2. Introduce a runtime facade beside terminal UI helpers. Done as
   `nxtrt::runtime`: it owns a `deck`, platform `wand`, root-firm run
   entrypoint, damage bell, input wire, resize wire, and `sleep`.
   `nxtrt::terminal_app` and the runtime demos layer terminal/compositor
   ownership on top of it.

3. Port buffer and HTTP helpers to `nxtrt::task`. Done in `src/nxtrt`.
   Keep request/response data structures free of runtime dependencies.
   `src/nxtai/responses_request.hpp` is the model for that split.

4. Make OpenAI streaming use `src` networking. Done for the one-shot
   `nxtllm` path: it connects over `nxtrt` TCP/TLS, reads HTTP/SSE, and
   writes text deltas to stdout.

5. Port tool execution after streaming works. The neutral tool-call pieces live
   in `src/nxtai/tool_batch.hpp`, `agent_tools.hpp`, and `tool_process.hpp`.
   The old runtime UI wrapper has been removed.

6. Re-enable `nxtllm` on the runtime. Done in
   `src/nxtai/nxtllm.cpp`: the executable builds by default, parses CLI
   options, enters `nxtrt::runtime`, and streams Responses text over the `src`
   HTTP/TLS stack.

7. Use PTYs for live tool surfaces. Started with `nxt-shell-scope-demo`: the
   command runs through `spawn_pty`, feeds a `vterm` session, and renders as a
   TUI surface. Cgroup sampling reads files through `openat`/`read_some`
   wishes and batches each sample with `when_all`.

## Compatibility strategy

Do not restore a compatibility alias layer. The old libcoro task and
`nxtrt::task` have different ownership and pump semantics, and a dual alias
layer would hide the hard parts. Prefer explicit ports:

- Move dependency-light data types first.
- Port leaf async functions next.
- Keep old UI demos only when they teach an unported subsystem; delete them
  once the runtime version covers the same behavior.
- Treat a clean default Meson build as the guardrail for runtime-only work.

## Remaining Work

The core migration is done. The remaining work is product shape:

1. Rebuild any richer interactive `nxtllm` UI as a separate observer over
   stream events instead of reviving the old UI runtime.
2. Decide which demos still carry their weight after the runtime consolidation.
3. Keep request construction and JSON parsing runtime-neutral.
4. Keep the default Meson build as the guardrail for runtime-only work.
