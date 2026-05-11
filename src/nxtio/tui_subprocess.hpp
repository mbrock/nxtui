#pragma once

#include "nxt/tui.hpp"
#include "nxt/tui_terminal.hpp"
#include "nxt/vterm.hpp"
#include "nxtio/subprocess.hpp"

namespace nxt::tui {

/// Layout adapter for a PTY-backed terminal session.
struct PtyScreen
{
    /// Session to render and resize; null falls back to `fallback`.
    nxt::subprocess::PtySession * session = nullptr;
    /// Terminal used when no live session is available.
    nxt::vterm::Terminal * fallback = nullptr;
    /// Style used to clear the pane before terminal contents are drawn.
    Style clear_style{};

    /// PTY screens grow to fill available width.
    constexpr WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    /// PTY screens grow to fill available height.
    constexpr HeightHint height_hint() const
    {
        return HeightHint::grow();
    }

    /// Resize the session and render its terminal under the session lock.
    void render(RasterView & raster, Size size) const
    {
        if (session != nullptr) {
            session->resize(size);
            session->with_terminal([&](nxt::vterm::Terminal & terminal) {
                render_vterm_screen(raster, size, terminal, clear_style);
            });
            return;
        }

        if (fallback != nullptr)
            render_vterm_screen(raster, size, *fallback, clear_style);
    }
};

/// Build a layout for a live PTY session.
inline auto pty_screen(
    nxt::subprocess::PtySession & session,
    Style clear_style = {})
{
    return PtyScreen{&session, nullptr, clear_style};
}

/// Build a layout that can fall back to a standalone terminal screen.
inline auto pty_screen(
    nxt::subprocess::PtySession * session,
    nxt::vterm::Terminal & fallback,
    Style clear_style = {})
{
    return PtyScreen{session, &fallback, clear_style};
}

} // namespace nxt::tui
