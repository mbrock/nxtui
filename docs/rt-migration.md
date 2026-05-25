# Runtime migration notes

The goal is to move the application/runtime surface from the old `nxtio` stack
onto `nxt::rt`.  `libcoro` has been removed from the Meson build and from
`subprojects`; remaining `nxtio` sources are porting reference material rather
than a build lane. The old custom task prototype has been removed; its useful
ideas now belong in `nxt::rt::env`, task zones, and explicit UI/runtime
capabilities.

## Current split

`src-ng` already owns the new coroutine substrate:

- `nxt::rt::task<T>` for lazy coroutine tasks.
- `nxt::rt::deck` for pumpable execution.
- `nxt::rt::wand` implementations for platform waiting.
- `nxt::rt::with_zone`, `fork`, `deed`, `when_all`, and timeout helpers.
- DNS, HTTP, TLS, and socket experiments.
- Core terminal input types and parsing in `src/nxt/input.hpp`, shared by
  both the ng runtime and the legacy `nxtio` shim.
- The default `nxtllm` executable, currently able to parse CLI options and
  construct OpenAI Responses request envelopes on `nxt::rt`.

The old application stack is no longer built.  It still contains useful UI and
tooling code, but `src/nxtio/async-core.hpp` aliases `nxt::task`,
`nxt::scheduler`, `nxt::queue`, `nxt::event`, `nxt::latch`, `sync_wait`, and
`when_all` to removed libcoro primitives. Most of the legacy UI, process,
signal, and old `nxtai` code enters async through those aliases.

## Migration table

| Old surface | New target | Notes |
| --- | --- | --- |
| `nxt::task<T>` | `nxt::rt::task<T>` | Start with leaf code that does not expose scheduler handles. |
| `nxt::scheduler` | `nxt::rt::deck` + `nxt::rt::wand` | Keep the host pumpable so terminal and Emacs embeddings can own the event loop. |
| `scheduler.yield_for(d)` | `nxt::rt::op::timeout::after(d)` or `with_timeout` | Keep sleep/yield as runtime methods at the app boundary. |
| `scheduler.poll(...)` | `nxt::rt::op::poll*` | Convert call sites once they are inside an `nxt::rt::task`. |
| `nxt::queue<T>` | `nxt::rt` channel primitive | This is the first missing primitive for UI input, resize, and tool streams. |
| `nxt::event` | `nxt::rt` event/condition primitive | Needed for damage notifications and small UI coordination points. |
| `nxtio/input.hpp` | `nxt/input.hpp` | Done. The old header is now only a compatibility include. |
| `nxt::latch` | zone join/deeds or a small latch | Prefer structured joins; add a latch only for true countdown cases. |
| `spawn_detached` | `nxt::rt::fork` in a zone | Detached work should still be owned by a root zone. |
| `nxt::scope` | `nxt::rt::task_zone` + UI capabilities | Scope currently mixes lifetime, scheduler, and yard state. Split those. |
| `nxtio/net` | `src-ng` HTTP/TLS/DNS | The OpenAI streaming path should use the new HTTP client directly. |
| old `src/nxtai/nxtllm.cpp` | `src-ng/nxtai/nxtllm.cpp` | Started. The executable now builds on `nxt::rt`; streaming/HUD/tools still need ports. |

## First code slices

1. Add `nxt::rt` channel and event primitives. Done as deck-local
   `nxt::rt::channel<T>` and manual-reset `nxt::rt::event`.
   `UIRuntime` needs input queues, resize queues, and damage notifications
   before it can stop depending on libcoro.

2. Introduce an ng runtime facade beside `nxt::ui::UIRuntime`. Started as
   `nxt::rt::runtime`: it owns a `deck`, platform `wand`, root-zone run
   entrypoint, damage event, input channel, resize channel, and `sleep`.
   The next part is to layer terminal/compositor ownership and yard-like
   surfaces on top of it.  `ng-tui-demo` is the first tiny compositor demo
   running through this path.

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

6. Re-enable `nxtllm` on the ng runtime. Started as
   `src-ng/nxtai/nxtllm.cpp`: the executable builds by default, parses CLI
   options, enters `nxt::rt::runtime`, and constructs Responses request JSON.
   Next success condition: stream a response over the `src-ng` HTTP/TLS stack.

## Compatibility strategy

Do not try to make `src/nxtio/async-core.hpp` alias both runtimes.  The old
libcoro task and `nxt::rt::task` have different ownership and pump semantics,
and a dual alias layer would hide the hard parts. Prefer explicit ports:

- Move dependency-light data types first.
- Port leaf async functions next.
- Keep old UI demos as source references until the app facade is ready.
- Treat a clean default Meson build as the guardrail for new-runtime-only work.

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
