# Runtime migration notes

The goal is to move the application/runtime surface from the old
`nxtio`/libcoro stack onto `nxt::rt`, then make `nxtllm` build in the
`-Dlegacy_runtime=false` configuration.  The old custom task prototype
has been removed; its useful ideas now belong in `nxt::rt::env`, task
zones, and explicit UI/runtime capabilities.

## Current split

`src-ng` already owns the new coroutine substrate:

- `nxt::rt::task<T>` for lazy coroutine tasks.
- `nxt::rt::deck` for pumpable execution.
- `nxt::rt::wand` implementations for platform waiting.
- `nxt::rt::with_zone`, `fork`, `deed`, `when_all`, and timeout helpers.
- DNS, HTTP, TLS, and socket experiments that do not require libcoro.

The old application stack still depends on libcoro through
`src/nxtio/async-core.hpp`.  That header aliases `nxt::task`,
`nxt::scheduler`, `nxt::queue`, `nxt::event`, `nxt::latch`,
`sync_wait`, and `when_all` to libcoro primitives.  Most of the UI,
process, signal, and `nxtai` code enters async through those aliases.

## Migration table

| Old surface | New target | Notes |
| --- | --- | --- |
| `nxt::task<T>` | `nxt::rt::task<T>` | Start with leaf code that does not expose scheduler handles. |
| `nxt::scheduler` | `nxt::rt::deck` + `nxt::rt::wand` | Keep the host pumpable so terminal and Emacs embeddings can own the event loop. |
| `scheduler.yield_for(d)` | `nxt::rt::op::timeout::after(d)` or `with_timeout` | Keep sleep/yield as runtime methods at the app boundary. |
| `scheduler.poll(...)` | `nxt::rt::op::poll*` | Convert call sites once they are inside an `nxt::rt::task`. |
| `nxt::queue<T>` | `nxt::rt` channel primitive | This is the first missing primitive for UI input, resize, and tool streams. |
| `nxt::event` | `nxt::rt` event/condition primitive | Needed for damage notifications and small UI coordination points. |
| `nxt::latch` | zone join/deeds or a small latch | Prefer structured joins; add a latch only for true countdown cases. |
| `spawn_detached` | `nxt::rt::fork` in a zone | Detached work should still be owned by a root zone. |
| `nxt::scope` | `nxt::rt::task_zone` + UI capabilities | Scope currently mixes lifetime, scheduler, and yard state. Split those. |
| `nxtio/net` | `src-ng` HTTP/TLS/DNS | The OpenAI streaming path should use the new HTTP client directly. |

## First code slices

1. Add `nxt::rt` channel and event primitives. Done as deck-local
   `nxt::rt::channel<T>` and manual-reset `nxt::rt::event`.
   `UIRuntime` needs input queues, resize queues, and damage notifications
   before it can stop depending on libcoro.

2. Introduce an ng runtime facade beside `nxt::ui::UIRuntime`.
   It should own a `deck`, a platform `wand`, a root zone, and the existing
   terminal/compositor state.  Keep the UI-facing methods familiar:
   `sleep`, `next_input`, `damage`, `spawn`, and `run`.

3. Port `nxtio/buffers.hpp` and `nxtio/http.hpp`-style helpers to
   `nxt::rt::task`.
   Keep request/response data structures free of runtime dependencies.
   `src/nxtai/responses_request.hpp` is the model for that split.

4. Make OpenAI streaming use `src-ng` networking.
   `nxtai/response_turn.hpp` currently enters through
   `runtime.scheduler_handle()` and `nxt::io::net::connect_tls`.  Replace
   that path with a small `nxt::rt` stream reader that yields parsed SSE
   events.

5. Port tool execution after streaming works.
   `nxtai/tools/subprocess.hpp`, `grep`, `bash`, and `web_fetch` take a
   scheduler pointer today.  Move those to a runtime capability that offers
   `poll`, `sleep`, and subprocess handling on `nxt::rt`.

6. Re-enable `nxtllm` under `-Dlegacy_runtime=false`.
   The success condition is that `meson setup -Dlegacy_runtime=false
   -Dllm_tool=true` builds `nxtllm` without pulling in libcoro.

## Compatibility strategy

Do not try to make `src/nxtio/async-core.hpp` alias both runtimes.  The
old libcoro task and `nxt::rt::task` have different ownership and pump
semantics, and a dual alias layer would hide the hard parts.  Prefer
explicit ports:

- Move dependency-light data types first.
- Port leaf async functions next.
- Keep old UI demos on the legacy runtime until the app facade is ready.
- Use `-Dlegacy_runtime=false` as the guardrail for new-runtime-only work.

## `nxtllm` path

The fastest path to a useful `nxtllm` migration is not the full terminal
UI first.  Start with the agent transport:

1. Keep OpenAI request construction and JSON parsing runtime-neutral.
2. Add an `nxt::rt` OpenAI stream reader over the `src-ng` HTTP/TLS stack.
3. Run one response turn in a root zone with no yard UI.
4. Add tool execution through `nxt::rt` subprocess support.
5. Add the terminal yard facade once input, damage, and child surfaces have
   ng runtime primitives.

That gets model streaming working early, then lets the richer UI follow
without blocking the core agent loop.
