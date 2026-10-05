# nxt

`nxt` is a set of C++23 libraries for coroutine-driven command-line software.
A terminal HUD, an HTTP/TLS request, a subprocess, and a streaming LLM call are
all tasks on the same cooperative scheduler.

```cpp
#include <nxtrt/app.hpp>
using namespace std::chrono_literals;

nxtrt::task<void> main_task()
{
    co_await nxtrt::op::timeout::after(30ms);
}

int main()
{
    auto rt = nxtrt::runtime{};
    rt.run(main_task);
}
```

It is also an experiment: a search for one model of concurrent, effectful
computation that is coherent, checkable, and cheap at once. The vocabulary
(`deck`, `wand`, `wish`, `hope`, ...) is deliberately provisional. See
[The quest][quest] for why.

## What's here

- [`nxtrt`][nxtrt] — the coroutine runtime: [tasks][nxtrt-task], the
  [deck][nxtrt-deck] scheduler, [wand][nxtrt-wand] I/O backends (%io_uring,
  epoll, kqueue, Windows/UWP IOCP), structured groups over a bounded [pool][nxtrt-pool], byte
  streams, [files][nxtrt-fs], [HTTP][nxtrt-http]/[TLS][nxtrt-tls],
  [WebSocket][nxtrt-websocket], and
  [subprocesses][nxtrt-subprocess].
- [`nxtui`][nxtui] — terminal rendering: typed geometry, styled text, rasters,
  and composable [layout values][nxtui-tui].
- [`nxtai`][nxtai] — an OpenAI Responses client and tool runner, behind the
  `nxtllm` executable ([status][ai-overview]).
- [Wisp][wisp] — a portable Lisp machine on the same scheduler, with
  checkpointable tapes, capability-scoped files, and an HTTP server and client.

## Where to read next

| Page | What it is |
| --- | --- |
| [A tour of the runtime][rt-overview] | Tasks, the deck, wishes and wands, cancellation, groups, pools, and streams, with working examples. Start here. |
| [Wisp][wisp] | The Lisp machine: the language, `await`, checkpoints and tapes, capabilities, HTTP. |
| [The quest][quest] | What the project is searching for, and the open questions. |
| [A story about holding work][rt-holding] | The essay behind the runtime: deck, wand, streams, and `hope<T>` as one idea. |
| [Runtime RFCs][runtime-rfcs] | The design notebook, current and speculative. |
| [Building and tooling][building] | Meson, Nix, CI, coverage, editors, the runtime spec, docs. |

## Quick start

Needs Meson 1.3+ and a C++23 compiler with `#embed` (GCC 15+ or Clang 19+).
Nix is optional (`nix develop` gives the full toolchain).

```sh
meson setup build
meson compile -C build
build/nxt-tests
build/demo/nxt-tui-demo
```

### Windows UWP / Xbox

Cross-build the coroutine core and IOCP backend on x86_64 Linux with the
[nixbox](https://github.com/mbrock/nixbox) flake's MSVC-ABI UWP toolchain:

```sh
nix build .#nxtrt-iocp
```

The package installs `nxtrt-iocp.lib`, portable runtime/networking/agent headers,
`nxtrt-iocp.pc`, and cross-linked IOCP, HTTP/TLS and WebSocket probes.
Meson selects IOCP automatically on Windows (`-Ddefault_wand=iocp` can force
it). DNS, socket wrappers, HTTP/TLS, ws/wss and the reusable Responses transport
are included; POSIX terminal/process/filesystem layers and Wisp/CLI tools are
not built by the Windows target. See [building instructions][building] for
the installed-consumer check and deterministic localhost fixtures.

Include `<nxtrt/wand/iocp.hpp>` and call `nxtrt::run_with_iocp(factory)`, or
attach `nxtrt::arch::wand` to a deck and use `poll(deck)` from a host event
loop. Supported wishes are `manual`, monotonic `timeout`, file/byte-pipe
`read_some`/`write_some`, socket `recv_some`/`send_some`, `connect` (ConnectEx),
and `accept` (AcceptEx). Readiness `poll`/`poll_until` explicitly fail; race
an actual I/O task with a timeout instead.

On Windows, file wishes borrow native overlapped `HANDLE`s and socket wishes
borrow pointer-width Winsock `SOCKET`s, not CRT descriptors. Disk operations
require explicit offsets. The host initializes Winsock, opens handles with
`FILE_FLAG_OVERLAPPED` / `WSA_FLAG_OVERLAPPED`, and closes them after all wishes
finish. Call `wand.attach(handle)` once per freshly opened handle before I/O
(accepted sockets are attached automatically). Reattaching fails, even to the
same port; newly opened handles need attachment even if their numeric value
was reused. A handle belongs to one wand's completion port; never enable
`FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`. Cancellation drains the operation
packet before resuming, and successful I/O still wins a racing stop request.
Failures use `std::system_error` with Windows/WSA codes. Tracing is host-enabled
via `nxtrt::trace_enabled`; UWP has no environment-based tracing or SIGUSR1 dump.

Apps still need nixbox's app-container packaging and appropriate manifest
capabilities. The loopback test runner is for desktop Windows (or Wine), not
an Xbox deployment: UWP loopback restrictions and console capabilities need
validation in a packaged app on hardware. Run it from a writable directory.

## Repository map

- `src/nxtrt`, `src/nxtui`, `src/nxtai` — the three libraries.
- `src/wisp` — the Lisp machine, moving heap, tapes, and executable host.
- `src/nxt` — shared protocol and utility code (crypto, TLS, JSON, PNG, ...).
- `nxtrt/runtime.rkt` — the formal model of the runtime (`make spec`).
- `demo`, `test`, `docs`, `rfc` — demos, nested test suites, doc pages, design notes.

<!-- Concept pages -->
[quest]: https://mbrock.github.io/nxtui/quest.html
[wisp]: https://mbrock.github.io/nxtui/wisp.html
[building]: https://mbrock.github.io/nxtui/building.html
[ai-overview]: https://mbrock.github.io/nxtui/ai_overview.html
[rt-overview]: https://mbrock.github.io/nxtui/rt_overview.html
[rt-holding]: https://mbrock.github.io/nxtui/rt_holding.html
[runtime-rfcs]: https://mbrock.github.io/nxtui/runtime_rfcs.html

<!-- nxtrt -->
[nxtrt]: https://mbrock.github.io/nxtui/namespacenxtrt.html
[nxtrt-task]: https://mbrock.github.io/nxtui/classnxtrt_1_1task.html
[nxtrt-deck]: https://mbrock.github.io/nxtui/classnxtrt_1_1deck.html
[nxtrt-wand]: https://mbrock.github.io/nxtui/classnxtrt_1_1wand.html
[nxtrt-pool]: https://mbrock.github.io/nxtui/classnxtrt_1_1pool.html
[nxtrt-fs]: https://mbrock.github.io/nxtui/namespacenxtrt_1_1fs.html
[nxtrt-http]: https://mbrock.github.io/nxtui/namespacenxtrt_1_1http.html
[nxtrt-tls]: https://mbrock.github.io/nxtui/namespacenxtrt_1_1tls.html
[nxtrt-websocket]: https://mbrock.github.io/nxtui/namespacenxtrt_1_1websocket.html
[nxtrt-subprocess]: https://mbrock.github.io/nxtui/namespacenxtrt_1_1subprocess.html

<!-- nxtui -->
[nxtui]: https://mbrock.github.io/nxtui/namespacenxtui.html
[nxtui-tui]: https://mbrock.github.io/nxtui/namespacenxtui_1_1tui.html

<!-- nxtai -->
[nxtai]: https://mbrock.github.io/nxtui/namespacenxtai.html
