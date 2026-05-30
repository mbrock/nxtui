# nxt

`nxt` is a set of C++ libraries for coroutine-driven command-line software. It
combines a structured async runtime, terminal rendering primitives, and an
LLM/tooling layer that can share the same runtime.

The public surface is organized around three root namespaces:

- [`nxtrt`][nxtrt] is the coroutine runtime for tasks, scheduling, structured
  child work, async I/O, subprocesses, and terminal app loops.
- [`nxtui`][nxtui] is the terminal/raster/layout toolkit for typed screen
  geometry, styled text, composable layout values, and terminal compositing.
- [`nxtai`][nxtai] is the OpenAI/LLM client and tool execution layer used by
  the `nxtllm` executable.

## nxtrt

[`nxtrt`][nxtrt] is the base runtime. It provides lazy coroutine
[`task<T>`][nxtrt-task] values, an explicit cooperative [`deck`][nxtrt-deck],
platform [`wand`][nxtrt-wand] backends, structured [`firm`][nxtrt-firm]
ownership, [`deed<T>`][nxtrt-deed] handles, [`channel<T>`][nxtrt-channel],
[`event`][nxtrt-event], and low-level awaitable operations under
[`nxtrt::op`][nxtrt-op].

Above the scheduler core, `nxtrt` also contains byte streams and protocol or
process helpers:

- [`nxtrt::fs`][nxtrt-fs] for async file helpers
- [`nxtrt::http`][nxtrt-http] and [`nxtrt::tls`][nxtrt-tls] for the HTTP/TLS
  client stack
- [`nxtrt::subprocess`][nxtrt-subprocess] and [`nxtrt::pty`][nxtrt-pty] for
  child process work
- [`nxtrt::terminal_app`][nxtrt-terminal-app] for terminal guest applications

Start with the [runtime overview][rt-overview] for the conceptual model.

```cpp
#include <nxtrt/app.hpp>

#include <chrono>

using namespace std::chrono_literals;

nxtrt::task<void> main_task()
{
    co_await nxtrt::op::timeout::after(30ms);
}

int main()
{
    auto rt = nxtrt::runtime{};
    rt.run(main_task());
}
```

## nxtui

[`nxtui`][nxtui] is the terminal UI and rendering library. Its values are useful
on their own: the UI layer defines the data model and rendering primitives,
while live terminal applications can use `nxtrt` to drive input, refresh, and
process output.

The core pieces are typed terminal geometry such as [`Size`][nxtui-size] and
[`Pos`][nxtui-pos], color and style types such as [`Rgba8`][nxtui-rgba],
[`Raster`][nxtui-raster] and [`RasterView`][nxtui-raster-view], the
[`GlyphTable`][nxtui-glyph-table], ANSI and input helpers, and composable layout
values under [`nxtui::tui`][nxtui-tui]. A
[`TerminalCompositor`][nxtui-terminal-compositor] renders those layout values
into terminal output.

```cpp
#include <nxtui/tui.hpp>

using namespace nxtui::tui;

auto view = column(
    text("build", fg(nxtui::Rgba8::cyan()) | bold),
    row(
        text("compile"),
        progress_bar(64.0 * nxtui::percent),
        text(" 64%")));
```

A common use is a partial terminal HUD: a fixed-height layout can live at the
bottom of a terminal while ordinary process output continues in the scrollback
above it.

## nxtai

[`nxtai`][nxtai] is the LLM/OpenAI layer. It builds OpenAI Responses requests,
streams server-sent events over the `nxtrt` HTTP/TLS stack, and provides the
small `nxtllm` executable.

Useful entry points include
[`nxtai::responses::openai_responses_request`][nxtai-request],
[`nxtai::tools::tool_registry`][nxtai-tool-registry], and the OpenAI event/data
types under [`nxtai::openai`][nxtai-openai].

```sh
build/nxtllm --dump-request "hello from nxtrt"
```

## Repository Map

- `src/nxtrt` contains the structured coroutine runtime.
- `src/nxtui` contains terminal, raster, compositor, input, and layout code.
- `src/nxtai` contains the LLM/OpenAI client code and `nxtllm`.
- `src/nxt` contains shared protocol and utility code that is not tied to one
  root namespace.
- `demo` contains small runtime, terminal, HTTP, and shell demos.
- `test` contains the nested `_test` suites.
- `docs` contains the generated API documentation source.

## Building

This repo builds with Meson and does not require Nix:

```sh
meson setup build
meson compile -C build
build/nxt-tests
```

The default build produces `nxt-tests`, `nxtllm`, `libnxt-core.a`, and the demo
programs. Try the small TUI demo with:

```sh
build/demo/nxt-tui-demo
```

Regenerate local API docs with:

```sh
make docs
```

Run the portable test subset on a FreeBSD VM with:

```sh
scripts/freebsd-vm init
scripts/freebsd-vm test
```

The helper uses libvirt, cloud-init, ssh, and rsync. It defaults to the official
FreeBSD 15.0 amd64 `BASIC-CLOUDINIT-ufs.qcow2.xz` image, stores local state
under `.cache/freebsd-vm`, and leaves the VM persistent for fast repeat runs.
The guest installs GCC 15 and uses `gcc15`/`g++15` for the test build. The host
needs `libvirt-daemon-system`, `virtinst`, and `cloud-image-utils`.

[nxtrt]: https://swa.sh/nxt/namespacenxtrt.html
[nxtrt-task]: https://swa.sh/nxt/classnxtrt_1_1task.html
[nxtrt-deck]: https://swa.sh/nxt/classnxtrt_1_1deck.html
[nxtrt-wand]: https://swa.sh/nxt/classnxtrt_1_1wand.html
[nxtrt-firm]: https://swa.sh/nxt/classnxtrt_1_1firm.html
[nxtrt-deed]: https://swa.sh/nxt/classnxtrt_1_1deed.html
[nxtrt-channel]: https://swa.sh/nxt/classnxtrt_1_1channel.html
[nxtrt-event]: https://swa.sh/nxt/classnxtrt_1_1event.html
[nxtrt-op]: https://swa.sh/nxt/namespacenxtrt_1_1op.html
[nxtrt-fs]: https://swa.sh/nxt/namespacenxtrt_1_1fs.html
[nxtrt-http]: https://swa.sh/nxt/namespacenxtrt_1_1http.html
[nxtrt-tls]: https://swa.sh/nxt/namespacenxtrt_1_1tls.html
[nxtrt-subprocess]: https://swa.sh/nxt/namespacenxtrt_1_1subprocess.html
[nxtrt-pty]: https://swa.sh/nxt/namespacenxtrt_1_1pty.html
[nxtrt-terminal-app]: https://swa.sh/nxt/classnxtrt_1_1terminal__app.html
[rt-overview]: https://swa.sh/nxt/rt_overview.html
[nxtui]: https://swa.sh/nxt/namespacenxtui.html
[nxtui-size]: https://swa.sh/nxt/structnxtui_1_1_size.html
[nxtui-pos]: https://swa.sh/nxt/structnxtui_1_1_pos.html
[nxtui-rgba]: https://swa.sh/nxt/structnxtui_1_1_rgba8.html
[nxtui-raster]: https://swa.sh/nxt/classnxtui_1_1_raster.html
[nxtui-raster-view]: https://swa.sh/nxt/classnxtui_1_1_raster_view.html
[nxtui-glyph-table]: https://swa.sh/nxt/classnxtui_1_1_glyph_table.html
[nxtui-tui]: https://swa.sh/nxt/namespacenxtui_1_1tui.html
[nxtui-terminal-compositor]: https://swa.sh/nxt/classnxtui_1_1tui_1_1_terminal_compositor.html
[nxtai]: https://swa.sh/nxt/namespacenxtai.html
[nxtai-request]: https://swa.sh/nxt/structnxtai_1_1responses_1_1openai__responses__request.html
[nxtai-tool-registry]: https://swa.sh/nxt/structnxtai_1_1tools_1_1tool__registry.html
[nxtai-openai]: https://swa.sh/nxt/namespacenxtai_1_1openai.html
