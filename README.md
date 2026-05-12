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

`nxtio` contains the runtime pieces: the terminal compositor, signal handling,
and coroutine scheduler integration. It is currently implemented on top of
`libcoro`; the intent is to keep the core layout/raster layer separate from
that runtime choice.

The high-level runner takes:

- an initial state
- a pure-ish `build_ui(state)` function returning a layout
- an async `update(runtime, state)` coroutine

```cpp
#include <nxtio/app.hpp>
#include <nxt/tui.hpp>

struct State {
    nxt::percent_t progress{0.0 * nxt::percent};
};

int main()
{
    using namespace std::chrono_literals;
    using namespace nxt::tui;

    return nxt::ui::run(
        State{},
        [](const State & state) {
            return column(
                text("working", fg(nxt::Rgba8::cyan()) | bold),
                progress_bar(state.progress)
            );
        },
        [](nxt::ui::UIRuntime & runtime, State & state) -> nxt::task<> {
            for (int i = 0; i <= 100; ++i) {
                state.progress = i * nxt::percent;
                runtime.signal_damage();
                co_await runtime.sleep(30ms);
            }

            runtime.request_shutdown();
        });
}
```

The runtime owns terminal state and exposes:

- `signal_damage()` to request a redraw
- `println()` to write ordinary log lines below the HUD
- `sleep()` and `scheduler()` for coroutine timing and I/O integration
- terminal size and shutdown state helpers

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
- `src/nxtio` contains the runtime and coroutine-facing I/O pieces.
- `test` contains raster and terminal compositor tests.
- `subprojects/libcoro` vendors the coroutine/runtime dependency.
- `vendor/mdspan` vendors the header-only mdspan implementation.
- `vendor/libvterm` is used by the terminal tests.

## Building

This repo builds with Meson and does not require Nix:

```sh
meson setup build
meson compile -C build
meson test -C build
```

The default build includes the `nxtdemo` UI demo binary and the test suite. The
`nxtllm` trace/debug tool is off by default because it pulls in nanoarrow IPC
support:

```sh
meson setup build -Dllm_tool=true
```

`nxtdemo` bundles all UI demos behind subcommands (`nxtdemo build_sim`,
`nxtdemo cgroup_browser`, `nxtdemo shell_scope`). When the Arrow C++ Dataset and
Parquet development packages are installed, `nxtdemo span_browser` is also
available for browsing a Hive-partitioned span archive.

The API is still in motion, but the intended direction is stable: small
composable layout values, a typed raster underneath, and a runtime that works
well for both full-screen TUIs and partial HUD-style displays.
