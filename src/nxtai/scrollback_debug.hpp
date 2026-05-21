#pragma once

#include <nxt/tui.hpp>

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <utility>

namespace nxt::ai {

template<nxt::tui::Layout Child>
struct ScrollbackBox
{
    Child child;
    nxt::tui::Style border_style{
        nxt::tui::fg(nxt::Rgba8{95, 105, 120}) | nxt::tui::faint};

    constexpr nxt::tui::WidthHint width_hint() const
    {
        return nxt::tui::WidthHint::grow();
    }

    constexpr nxt::tui::HeightHint height_hint() const
    {
        return nxt::tui::HeightHint::fixed(
            child.height_hint().min + 2 * nxt::ln);
    }

    void render(nxt::RasterView & raster, nxt::Size size) const
    {
        using namespace nxt;
        std::ranges::fill(raster.glyphs(), 32);

        auto cols = size.w.count();
        auto rows = size.h.count();
        if (cols == 0 || rows == 0)
            return;

        auto draw = [&](std::size_t x, std::size_t y, std::string_view s) {
            auto pos = Pos::at(x * ch, y * ln);
            raster.write_text(pos, s);
            raster.set_fg(pos, border_style.fg);
            raster.set_em(pos, border_style.em);
        };

        draw(0, 0, "╭");
        if (cols > 1)
            draw(cols - 1, 0, "╮");
        if (rows > 1) {
            draw(0, rows - 1, "╰");
            if (cols > 1)
                draw(cols - 1, rows - 1, "╯");
        }

        for (std::size_t x = 1; x + 1 < cols; ++x) {
            draw(x, 0, "─");
            if (rows > 1)
                draw(x, rows - 1, "─");
        }
        for (std::size_t y = 1; y + 1 < rows; ++y) {
            draw(0, y, "│");
            if (cols > 1)
                draw(cols - 1, y, "│");
        }

        if (cols <= 4 || rows <= 2)
            return;

        auto content_size = Size{(cols - 4) * ch, (rows - 2) * ln};
        auto content = nxt::tui::subraster(
            raster, Pos::at(2 * ch, 1 * ln), content_size);
        child.render(content, content_size);
    }
};

template<nxt::tui::Layout Child>
auto scrollback_box(Child && child)
{
    return ScrollbackBox<std::decay_t<Child>>{std::forward<Child>(child)};
}

} // namespace nxt::ai
