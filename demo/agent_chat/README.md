# Graphical agent-chat slice

This is a fixture-driven SDL3 **Renderer** + SDL3_ttf/HarfBuzz sample. It
does not use SDL_GPU, a glyph raster, networking, or credentials. The existing
terminal toolkit remains available unchanged.

## Build and run

Use C++23 and pkg-config modules `sdl3` and `sdl3-ttf` (both at least 3.2):

```sh
nix develop .#gcc
meson setup build-graphical -Dsdl_ui=enabled -Ddev=false
meson compile -C build-graphical nxt-agent-chat-demo nxtui-graphical-tests
build-graphical/demo/agent_chat/nxt-agent-chat-demo --font /path/to/DejaVuSans.ttf
```

For an existing Meson directory, reconfigure with
`-Dpkg_config_path="$PKG_CONFIG_PATH"` if its cached dependency search path
predates SDL. `-Dsdl_ui=disabled` keeps SDL optional. `nix build
.#nxt-graphical` enables it and installs DejaVu Sans; the installed demo finds
that font in `../share/nxtui` relative to its executable. Other packaging
should bundle a font and pass its asset-relative path with `--font`.

Enter sends, Shift+Enter inserts a newline, Esc or Stop stops the fixture
stream, and the mouse wheel scrolls the transcript. SDL text-input/preedit
events and clipboard paste are supported. This is deliberately a small
composer, not a selection/cursor-navigation or grapheme-editing framework.

## Public layout/paint contract

`<nxtui/ui.hpp>` supplies renderer-neutral value compositions:

```cpp
namespace ui = nxtui::ui;
auto content = ui::surface(ui::bg(nxtui::Rgba8(24, 34, 48)),
    ui::padding(ui::Insets::symmetric(1.5 * ui::ch, 0.75 * ui::ln),
        ui::column(0.25 * ui::ln,
            ui::text("Title", ui::bold),
            ui::text("Proportional, shaped, wrapping text."))));
auto frame = ui::paint(content, painter, painter.viewport());
painter.draw(frame);
```

A layout implements `measure(context, available_width)` and
`paint(context)`. Width/height are distinct continuous rhythm types, including
halves, quarters, and multiples. Text uses the backend's measured advances;
one `ch` is the regular font's shaped `0` advance, and one `ln` its line skip,
not a claim that each codepoint occupies one cell. Stack allocation, child
visitation, and styles are shared with the terminal compositions rather than
copied into a second widget framework. Fixed-width boxes constrain wrapping
before height measurement; only positive leftover space is flex-distributed.

`Frame` owns a display list of `FillRect`, `StrokeRect`, and `GlyphRun` with
logical clips. Clipping preserves assigned geometry (including offscreen
scroll origins); hit rectangles have half-open edges. SDL alone converts to
pixels and rounds clip endpoints. Strokes use character-unit thickness in
both directions. `SdlPainter` borrows an SDL_Renderer, owns font variants and
the SDL_ttf text engine, and shapes/paints on the UI thread. Destroy frames and
shaped runs before the painter, the painter before the renderer, and all of
them before TTF/SDL shutdown. Bold, italic, underline, strike, faint, conceal
and reverse are supported; this slice does not animate terminal blink.

## Transport and Windows/Xbox integration

`view.hpp` is the sample's transport-independent seam: `chat::State` holds
messages, composer/preedit, status/detail, and scroll position. Update that
state on the UI thread, append streamed deltas to an assistant message, then
set Ready/Stopped/Error on completion. `chat::view(state, hits)` builds the
same public compositions. Replace `Fixture::tick/send/stop` in `main.cpp`
with your transport scheduling and cancellation; do not call SDL or mutate
state from a transport worker. The sample neither reads an API key nor
chooses trust roots.

`src/nxtui/meson.build` is independently invokable: it needs only the root's
C++23 project and `sdl_ui` feature option, and exposes
`nxtui_sdl_available`/`nxtui_sdl_dep`. It installs `nxtui-sdl.pc` and the
portable public headers. `demo/agent_chat/meson.build` consumes that dependency
and the existing `tests` option. The Windows early branch can call these two
subdirectories directly, after `src/nxtrt/wand`, without POSIX nxt-core,
Wisp, libvterm, or a dependency on `src/meson.build`. Honor `demo` around the
sample call. The normal `main` includes SDL_main.h and uses the Windows GUI
subsystem. Xbox packaging and on-device verification belong to nixbox.

## Checks and reproducible native states

```sh
meson test -C build-graphical graphical-layout --print-errorlogs
build-graphical/demo/agent_chat/nxtui-graphical-tests /path/to/DejaVuSans.ttf
build-graphical/demo/agent_chat/nxt-agent-chat-demo --font /path/to/DejaVuSans.ttf --exercise
build-graphical/demo/agent_chat/nxt-agent-chat-demo --font /path/to/DejaVuSans.ttf --state ready --snapshot ready.bmp
build-graphical/demo/agent_chat/nxt-agent-chat-demo --font /path/to/DejaVuSans.ttf --state error --width 540 --height 640 --snapshot error.bmp
```

The default test runs fractional geometry without fonts/video. A supplied
proportional font additionally tests native shaping, composed/decomposed
accents, wrapping, every pixel of a fractional clip, restoration of a caller
clip, and nonoverlapping chat hit regions. `--exercise` injects actual SDL
UTF-8 input, backspace, Enter and Escape events, synchronizes a native resize,
and checks send/stop/fixture completion. Snapshot states also include
`streaming` and `stopped`; snapshots freeze fixture time. For headless native
checks use an Xvfb display or `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software`.
