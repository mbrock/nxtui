#pragma once

#include "nxt/tui.hpp"
#include "nxt/tui_terminal.hpp"
#include "nxt/vterm.hpp"
#include "nxtio/subprocess.hpp"

namespace nxt::tui {

struct PtyScreen
{
    nxt::subprocess::PtySession * session = nullptr;
    nxt::vterm::Terminal * fallback = nullptr;
    Style clear_style{};

    constexpr WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    constexpr HeightHint height_hint() const
    {
        return HeightHint::grow();
    }

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

inline auto pty_screen(
    nxt::subprocess::PtySession & session,
    Style clear_style = {})
{
    return PtyScreen{&session, nullptr, clear_style};
}

inline auto pty_screen(
    nxt::subprocess::PtySession * session,
    nxt::vterm::Terminal & fallback,
    Style clear_style = {})
{
    return PtyScreen{session, &fallback, clear_style};
}

} // namespace nxt::tui
