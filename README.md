# nxt

`nxt` is a small terminal UI library extracted from `nixb`.

The goal is a library that is pleasant to use from C++, compositional enough for
real interfaces, and fast enough for frequently updated terminal displays. It is
not trying to be a full widget toolkit. The current shape is closer to a typed
raster plus a small declarative layout layer and a coroutine-friendly runtime.

One important use case is a partial terminal HUD: a live interface at the top of
the terminal while ordinary log output continues below it. This is useful for
build monitors and other tools where the UI should summarize what is happening
without swallowing the primary scrollback.

## Layout Model

Layouts are ordinary C++ values. A layout reports:

- a minimum width and height
- whether it wants flexible extra space
- how to render itself into a `RasterView`

Most UI is built by composing small values from `nxt::tui`:

```cpp
#include <nxt/tui.hpp>

using namespace nxt::tui;

auto view = row(
    text("llvm", fg(nxt::Rgba8::blue()) | bold),
    progress_bar(42.0 * nxt::percent),
    text(" 42%", fg(nxt::Rgba8::white()))
);
```

The layout functions return concrete values, so composition is type checked and
does not require an object hierarchy.

## Basic Primitives

Text:

```cpp
text("hello")
text("warning", fg(nxt::Rgba8::yellow()) | bold)
styled_text(
    span("build ", fg(nxt::Rgba8::white())),
    span("failed", fg(nxt::Rgba8::red()) | bold)
)
```

Rules, fills, and progress:

```cpp
hrule()
fill(nxt::Rgba8(24, 24, 24))
progress_bar(73.0 * nxt::percent)
progress_bar(73.0 * nxt::percent, nxt::Rgba8::green())
```

Horizontal and vertical composition:

```cpp
row(
    text("fetch"),
    progress_bar(18.0 * nxt::percent),
    text(" 18%")
)

column(
    text("building nixpkgs#hello", fg(nxt::Rgba8::cyan()) | bold),
    hrule(),
    row(text("compile"), progress_bar(64.0 * nxt::percent))
)
```

Dynamic lists:

```cpp
struct Job {
    std::string name;
    nxt::percent_t progress;
};

std::vector<Job> jobs = /* ... */;

auto jobs_view = list(jobs, [](const Job & job) {
    return row(
        text(fmt::format("{:<24}", job.name)),
        progress_bar(job.progress),
        text(fmt::format(
            " {:>3.0f}%",
            job.progress.value()))
    );
});
```

## Running an App

New application work should start on `nxt::rt`, the structured coroutine
runtime in `src`. It owns a `deck`, a platform I/O wand, a root task zone,
and small app-facing queues for input, resize, and damage notifications.

The current smallest TUI entry point is `nxt-tui-demo`: it renders a real
terminal compositor frame from an `nxt::rt::runtime` task and animates with
runtime sleeps.

```cpp
#include <nxt/tui.hpp>
#include <nxt/rt/app.hpp>

int main()
{
    using namespace std::chrono_literals;
    using namespace nxt::tui;

    auto runtime = nxt::rt::runtime{};
    runtime.run([]() -> nxt::rt::task<void> {
        for (int i = 0; i <= 100; ++i) {
            // Render a layout through TerminalCompositor here.
            co_await nxt::rt::op::timeout::after(30ms);
        }
    });
}
```

The old `nxtio` runner sources have been removed; the Meson build no longer
vendors or builds `libcoro`.

The runtime owns:

- `signal_damage()` and `damage_event()` for redraw coordination
- input and resize channels
- `sleep()` and root-zone `run()` entry points
- the platform wand used by lower-level async file, socket, DNS, TLS, and HTTP
  operations

## Partial HUD Behavior

The compositor renders the layout into a reserved region at the top of the
terminal. If the layout has a fixed height, `nxt` treats it as a HUD and leaves
space below for normal output:

```cpp
runtime.println("downloaded narinfo");
runtime.println("building /nix/store/...");
```

Those lines remain in the terminal scrollback while the HUD is redrawn above
them. If a layout asks to grow vertically, the runtime treats it as a full-screen
view.

This distinction is deliberate: many command-line tools need both a structured
summary and the raw stream of details.

## Typed Coordinates

The raster layer uses small typed terminal units:

```cpp
auto size = nxt::Size{80 * nxt::ch, 24 * nxt::ln};
auto pos = nxt::Pos::at(2 * nxt::ch, 1 * nxt::ln);
```

This keeps columns, rows, sizes, and percentages from collapsing into anonymous
integers throughout layout and rendering code.

## Structure

- `src/nxt` contains core terminal, raster, units, and layout code.
- `src/nxt/rt` contains the new structured coroutine runtime.
- `src/nxt/llm` contains the runtime `nxtllm` entry point.
- `test` contains raster, terminal compositor, and runtime tests.
- `vendor/mdspan` vendors the header-only mdspan implementation.
- `vendor/libvterm` is used by the terminal tests.

## Building

This repo builds with Meson and does not require Nix:

```sh
meson setup build
meson compile -C build
meson test -C build
```

The default build is the runtime lane: it builds core layout/raster code,
`nxt-tests`, `nxtllm`, and the runtime demos without pulling in `libcoro`. Try the
small TUI demo with:

```sh
build/demo/nxt-tui-demo
```

`nxtllm` is also an target now. It can parse the familiar CLI and construct
OpenAI Responses requests on `nxt::rt`; network streaming and the richer HUD are
the next migration slices:

```sh
build/nxtllm --dump-request "hello from the new runtime"
```

Install `cpptrace` if you want richer crash stack traces in tests. Meson enables
it only after a real compile/link probe, so compilers that find an incompatible
system package will fall back to the built-in stacktrace shim.

The API is still in motion, but the intended direction is stable: small
composable layout values, a typed raster underneath, and a runtime that works
well for both full-screen TUIs and partial HUD-style displays.
