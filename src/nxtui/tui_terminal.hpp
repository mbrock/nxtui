#pragma once

#include "nxtui/raster.hpp"
#include "nxtui/tui.hpp"
#include "nxtui/units.hpp"
#include "nxtui/vterm.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

namespace nxtui::tui {

namespace detail {

inline void append_utf8(std::string & out, std::uint32_t cp)
{
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

inline std::string cell_text(const nxtui::vterm::Cell & cell)
{
    std::string out;
    for (auto cp : cell.chars)
        append_utf8(out, cp);
    return out;
}

inline Rgba8 color_from_vterm(const nxtui::vterm::Color & color)
{
    if (color.is_default_fg() || color.is_default_bg())
        return DEFAULT_COLOR;
    if (color.is_rgb())
        return Rgba8(
            color.c.rgb.red,
            color.c.rgb.green,
            color.c.rgb.blue);
    if (color.is_indexed())
        return Rgba8::palette(color.c.indexed.idx);
    return DEFAULT_COLOR;
}

inline Emphasis emphasis_from_cell(const nxtui::vterm::Cell & cell)
{
    auto em = DEFAULT_EMPHASIS;
    if (cell.bold)
        em |= Emphasis::bold;
    if (cell.italic)
        em |= Emphasis::italic;
    if (cell.underline)
        em |= Emphasis::underline;
    if (cell.blink)
        em |= Emphasis::blink;
    if (cell.reverse)
        em |= Emphasis::reverse;
    if (cell.strike)
        em |= Emphasis::strikethrough;
    return em;
}

inline Rgba8 cursor_color_or(Rgba8 color, Rgba8 fallback)
{
    return color == DEFAULT_COLOR ? fallback : color;
}

inline void render_cursor(
    RasterView & raster,
    Size size,
    const nxtui::vterm::Cursor & cursor,
    Style clear_style)
{
    if (!cursor.visible || cursor.row < 0 || cursor.col < 0)
        return;
    if (cursor.row >= static_cast<int>(size.h.count())
        || cursor.col >= static_cast<int>(size.w.count()))
        return;

    auto pos = Pos::at(
        static_cast<std::size_t>(cursor.col) * ch,
        static_cast<std::size_t>(cursor.row) * ln);
    auto cell = raster.get_cell(pos);
    if (!cell)
        return;

    auto fg = cursor_color_or(cell->fg, clear_style.fg);
    auto bg = cursor_color_or(cell->bg, clear_style.bg);
    fg = cursor_color_or(fg, Rgba8::white());
    bg = cursor_color_or(bg, Rgba8::black());

    if (fg == bg) {
        fg = Rgba8::black();
        bg = Rgba8::bright_white();
    }

    raster.set_fg(pos, bg);
    raster.set_bg(pos, fg);
}

struct NoResize
{
    void operator()(Size) const noexcept {}
};

} // namespace detail

/// Clear a raster and render a libvterm screen into it.
inline void render_vterm_screen(
    RasterView & raster,
    Size size,
    nxtui::vterm::Terminal & terminal,
    Style clear_style = {});

/// Layout adapter that renders a `nxtui::vterm::Terminal`.
///
/// `ResizeFn`, when supplied, is called with the raster size before rendering
/// so PTY-backed terminals can stay in sync with their pane.
template<typename ResizeFn = detail::NoResize>
struct VTermScreen
{
    /// Terminal to render; null renders nothing.
    nxtui::vterm::Terminal * terminal = nullptr;
    /// Optional resize hook run before drawing.
    ResizeFn resize;
    /// Style used to clear the pane before terminal cells are drawn.
    Style clear_style{};

    /// Terminal screens grow to fill available width.
    constexpr WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    /// Terminal screens grow to fill available height.
    constexpr HeightHint height_hint() const
    {
        return HeightHint::grow();
    }

    /// Resize, clear, and render the terminal into `raster`.
    void render(RasterView & raster, Size size) const
    {
        if (terminal == nullptr || size.w == 0 * ch || size.h == 0 * ln)
            return;

        resize(size);
        render_vterm_screen(raster, size, *terminal, clear_style);
    }
};

/// Clear a raster and render terminal cells, colors, emphasis, and cursor.
inline void render_vterm_screen(
    RasterView & raster,
    Size size,
    nxtui::vterm::Terminal & terminal,
    Style clear_style)
{
    std::ranges::fill(raster.glyphs(), 32);
    std::ranges::fill(raster.fgs(), clear_style.fg);
    std::ranges::fill(raster.bgs(), clear_style.bg);
    std::ranges::fill(raster.ems(), clear_style.em);

    auto [term_rows, term_cols] = terminal.get_size();
    const auto rows =
        std::min<int>(term_rows, static_cast<int>(size.h.count()));
    const auto cols =
        std::min<int>(term_cols, static_cast<int>(size.w.count()));

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols;) {
            auto cell = terminal.get_cell(row, col);
            if (!cell) {
                ++col;
                continue;
            }

            const int cell_width = std::clamp(cell->width, 1, cols - col);
            auto text = detail::cell_text(*cell);
            auto pos = Pos::at(
                static_cast<std::size_t>(col) * ch,
                static_cast<std::size_t>(row) * ln);

            if (text.empty())
                raster.set_char(pos, ' ');
            else
                raster.write_text(pos, text);

            const auto fg = (cell->fg.is_default_fg()
                             || cell->fg.is_default_bg())
                ? clear_style.fg
                : detail::color_from_vterm(cell->fg);
            const auto bg = (cell->bg.is_default_fg()
                             || cell->bg.is_default_bg())
                ? clear_style.bg
                : detail::color_from_vterm(cell->bg);
            const auto em =
                clear_style.em | detail::emphasis_from_cell(*cell);

            for (int dx = 0; dx < cell_width; ++dx) {
                auto styled_pos = Pos::at(
                    static_cast<std::size_t>(col + dx) * ch,
                    static_cast<std::size_t>(row) * ln);
                raster.set_fg(styled_pos, fg);
                raster.set_bg(styled_pos, bg);
                raster.set_em(styled_pos, em);
            }

            col += cell_width;
        }
    }

    if (auto cursor = terminal.cursor())
        detail::render_cursor(raster, size, *cursor, clear_style);
}

/// Build a growable terminal layout without a resize hook.
inline auto vterm_screen(nxtui::vterm::Terminal & terminal)
{
    return VTermScreen<detail::NoResize>{&terminal, {}, {}};
}

/// Build a growable terminal layout with a custom clear style.
inline auto vterm_screen(nxtui::vterm::Terminal & terminal, Style clear_style)
{
    return VTermScreen<detail::NoResize>{&terminal, {}, clear_style};
}

/// Build a growable terminal layout with a resize hook.
template<typename ResizeFn>
    requires(!std::same_as<std::decay_t<ResizeFn>, Style>)
auto vterm_screen(nxtui::vterm::Terminal & terminal, ResizeFn && resize)
{
    return VTermScreen<std::decay_t<ResizeFn>>{
        &terminal,
        std::forward<ResizeFn>(resize),
        {}};
}

/// Build a growable terminal layout with a resize hook and clear style.
template<typename ResizeFn>
auto vterm_screen(
    nxtui::vterm::Terminal & terminal,
    ResizeFn && resize,
    Style clear_style)
{
    return VTermScreen<std::decay_t<ResizeFn>>{
        &terminal,
        std::forward<ResizeFn>(resize),
        clear_style};
}

} // namespace nxtui::tui
