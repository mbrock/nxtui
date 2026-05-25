#pragma once

#include "nxt/raster.hpp"
#include "nxt/chart.hpp"
#include "nxt/units.hpp"
#include "nxt/utf8.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxt::tui {

/// Maps a logical size unit to its strongly typed extent.
template<auto Unit>
struct hint_extent;

template<>
struct hint_extent<ch>
{
    using type = width_t;
};

template<>
struct hint_extent<ln>
{
    using type = height_t;
};

template<auto Unit>
using hint_extent_t = typename hint_extent<Unit>::type;

/// Minimum size plus flex-grow factor for one layout axis.
template<auto Unit>
struct SizeHint
{
    hint_extent_t<Unit> min{0 * Unit}; // Minimum size needed
    ratio_t flex{0.0 * one};           // Flex grow factor (0 = don't grow)

    /// Require exactly `n` minimum cells/lines and no growth.
    static constexpr SizeHint fixed(hint_extent_t<Unit> n)
    {
        return {n, 0.0 * one};
    }

    /// Ask to share remaining space using `factor` as the flex weight.
    static constexpr SizeHint grow(ratio_t factor = 1.0 * one)
    {
        return {0 * Unit, factor};
    }
};

/// Width hint measured in terminal cells.
using WidthHint = SizeHint<ch>;
/// Height hint measured in terminal lines.
using HeightHint = SizeHint<ln>;

/// Concept implemented by values that can report size hints and render.
template<typename L>
concept Layout =
    requires(const L & layout, RasterView & raster, Size size) {
        { layout.width_hint() } -> std::convertible_to<WidthHint>;
        { layout.height_hint() } -> std::convertible_to<HeightHint>;
        { layout.render(raster, size) } -> std::same_as<void>;
    };

/// Leaf layout backed by a render callback.
template<typename RenderFn>
struct Leaf
{
    /// Width hint returned to parents.
    WidthHint w_hint;
    /// Height hint returned to parents.
    HeightHint h_hint;
    /// Callback invoked when the leaf is rendered.
    RenderFn render_fn;

    /// Return this leaf's width hint.
    constexpr WidthHint width_hint() const
    {
        return w_hint;
    }

    /// Return this leaf's height hint.
    constexpr HeightHint height_hint() const
    {
        return h_hint;
    }

    /// Render by calling `render_fn`.
    void render(RasterView & raster, Size size) const
    {
        render_fn(raster, size);
    }
};

/// Create a callback-backed leaf layout.
template<typename F>
auto leaf(WidthHint w, HeightHint h, F && f)
{
    return Leaf<std::decay_t<F>>{w, h, std::forward<F>(f)};
}

/// Empty layout used when a typed composition needs an absent child.
inline auto empty()
{
    return leaf(WidthHint{}, HeightHint{}, [](RasterView &, Size) {});
}

/// Conditional layout that delegates to one of two child layouts.
template<Layout FalseLayout, Layout TrueLayout>
struct Either
{
    /// Render `true_layout` when set, otherwise `false_layout`.
    bool choose_true = false;
    /// Layout used when `choose_true` is false.
    FalseLayout false_layout;
    /// Layout used when `choose_true` is true.
    TrueLayout true_layout;

    /// Width hint of the selected child.
    constexpr WidthHint width_hint() const
    {
        return choose_true ? true_layout.width_hint()
                           : false_layout.width_hint();
    }

    /// Height hint of the selected child.
    constexpr HeightHint height_hint() const
    {
        return choose_true ? true_layout.height_hint()
                           : false_layout.height_hint();
    }

    /// Render the selected child.
    void render(RasterView & raster, Size size) const
    {
        if (choose_true)
            true_layout.render(raster, size);
        else
            false_layout.render(raster, size);
    }
};

/// Create a conditional layout from two alternatives.
template<Layout FalseLayout, Layout TrueLayout>
auto either(
    bool choose_true,
    FalseLayout && false_layout,
    TrueLayout && true_layout)
{
    return Either<std::decay_t<FalseLayout>, std::decay_t<TrueLayout>>{
        choose_true,
        std::forward<FalseLayout>(false_layout),
        std::forward<TrueLayout>(true_layout),
    };
}

/// Write UTF-8 text into a raster.
inline col_t write_text(RasterView & r, Pos pos, std::string_view text)
{
    return r.write_text(pos, text);
}

/// Set one cell's foreground color.
inline void set_fg(RasterView & r, Pos pos, Rgba8 color)
{
    r.set_fg(pos, color);
}

/// Set one cell's background color.
inline void set_bg(RasterView & r, Pos pos, Rgba8 color)
{
    r.set_bg(pos, color);
}

/// Create a child raster view relative to a parent view.
inline RasterView subraster(RasterView & r, Pos pos, Size size)
{
    return r.subraster(pos, size);
}

/// Foreground, background, and emphasis style overlay.
struct Style
{
    /// Foreground color, or `DEFAULT_COLOR` to inherit/reset.
    Rgba8 fg = DEFAULT_COLOR;
    /// Background color, or `DEFAULT_COLOR` to inherit/reset.
    Rgba8 bg = DEFAULT_COLOR;
    /// Emphasis bitset.
    Emphasis em = DEFAULT_EMPHASIS;

    /// Merge styles, letting explicit colors in `other` override this
    /// style.
    constexpr Style operator|(const Style & other) const
    {
        return {
            other.fg != DEFAULT_COLOR ? other.fg : fg,
            other.bg != DEFAULT_COLOR ? other.bg : bg,
            em | other.em,
        };
    }
};

/// Build a style that sets only foreground color.
constexpr Style fg(Rgba8 color)
{
    return {color, DEFAULT_COLOR, DEFAULT_EMPHASIS};
}

/// Build a style that sets only background color.
constexpr Style bg(Rgba8 color)
{
    return {DEFAULT_COLOR, color, DEFAULT_EMPHASIS};
}

/// Build a style that sets only emphasis flags.
constexpr Style em(Emphasis e)
{
    return {DEFAULT_COLOR, DEFAULT_COLOR, e};
}

/// Predefined emphasis-only styles.
inline constexpr Style bold{DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::bold};
inline constexpr Style faint{DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::faint};
inline constexpr Style italic{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::italic};
inline constexpr Style underline{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::underline};
inline constexpr Style reverse{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::reverse};
inline constexpr Style strikethrough{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::strikethrough};

/// Styled text segment used by `styled_text`.
struct Span
{
    /// UTF-8 text.
    std::string text;
    /// Style applied to the text.
    Style style{};
};

/// Create a styled text segment.
inline Span span(std::string text, Style s = {})
{
    return {std::move(text), s};
}

/// Layout decorator that clears its raster before rendering a child.
template<Layout Child>
struct Surface
{
    /// Style used for every cell in the clear pass.
    Style style{};
    /// Child rendered after the clear pass.
    Child child;

    /// Forward the child's width hint.
    constexpr WidthHint width_hint() const
    {
        return child.width_hint();
    }

    /// Forward the child's height hint.
    constexpr HeightHint height_hint() const
    {
        return child.height_hint();
    }

    /// Clear the full raster and render the child.
    void render(RasterView & raster, Size size) const
    {
        std::ranges::fill(raster.glyphs(), 32);
        std::ranges::fill(raster.fgs(), style.fg);
        std::ranges::fill(raster.bgs(), style.bg);
        std::ranges::fill(raster.ems(), style.em);
        child.render(raster, size);
    }
};

/// Create a clearing surface around a child layout.
template<Layout Child>
auto surface(Style style, Child && child)
{
    return Surface<std::decay_t<Child>>{style, std::forward<Child>(child)};
}

/// Layout decorator that forces a fixed height hint.
template<Layout Child>
struct FixedHeight
{
    /// Height reported to parent columns and HUD sizing.
    height_t height{0 * ln};
    /// Child rendered with whatever size the parent assigns.
    Child child;

    /// Forward the child's width hint.
    constexpr WidthHint width_hint() const
    {
        return child.width_hint();
    }

    /// Return the fixed height hint.
    constexpr HeightHint height_hint() const
    {
        return HeightHint::fixed(height);
    }

    /// Render the child without additional clipping behavior.
    void render(RasterView & raster, Size size) const
    {
        child.render(raster, size);
    }
};

/// Create a layout wrapper that reports a fixed height.
template<Layout Child>
auto fixed_height(height_t height, Child && child)
{
    return FixedHeight<std::decay_t<Child>>{
        height, std::forward<Child>(child)};
}

/// Layout decorator that forces a fixed width hint.
template<Layout Child>
struct FixedWidth
{
    /// Width reported to parent rows and HUD sizing.
    width_t width{0 * ch};
    /// Child rendered with whatever size the parent assigns.
    Child child;

    /// Return the fixed width hint.
    constexpr WidthHint width_hint() const
    {
        return WidthHint::fixed(width);
    }

    /// Forward the child's height hint.
    constexpr HeightHint height_hint() const
    {
        return child.height_hint();
    }

    /// Render the child without additional clipping behavior.
    void render(RasterView & raster, Size size) const
    {
        child.render(raster, size);
    }
};

/// Create a layout wrapper that reports a fixed width.
template<Layout Child>
auto fixed_width(width_t width, Child && child)
{
    return FixedWidth<std::decay_t<Child>>{
        width, std::forward<Child>(child)};
}

/// Layout decorator that lets a child claim remaining row width.
template<Layout Child>
struct GrowWidth
{
    Child child;
    ratio_t factor{1.0 * one};

    constexpr WidthHint width_hint() const
    {
        auto hint = child.width_hint();
        hint.flex = std::max(hint.flex, factor);
        return hint;
    }

    constexpr HeightHint height_hint() const
    {
        return child.height_hint();
    }

    void render(RasterView & raster, Size size) const
    {
        child.render(raster, size);
    }
};

/// Keep the child's minimum width but make it participate in row flex.
template<Layout Child>
auto grow_width(Child && child, ratio_t factor = 1.0 * one)
{
    return GrowWidth<std::decay_t<Child>>{
        std::forward<Child>(child), factor};
}

/// Render one styled span and return the column after the written text.
inline col_t render_span(RasterView & r, Pos pos, const Span & s)
{
    const auto start_x = pos.x;
    const auto end_x = r.write_text(pos, s.text);

    for (auto x = start_x; x < end_x; x += 1 * ch) {
        const Pos p{x, pos.y};
        if (s.style.fg != DEFAULT_COLOR)
            r.set_fg(p, s.style.fg);
        if (s.style.bg != DEFAULT_COLOR)
            r.set_bg(p, s.style.bg);
        if (s.style.em != DEFAULT_EMPHASIS)
            r.set_em(p, s.style.em);
    }

    return end_x;
}

/// Clear a one-line raster and apply explicitly-set style channels.
inline void clear_line(RasterView & r, Style style = {})
{
    std::ranges::fill(r.glyphs(), 32);
    if (style.fg != DEFAULT_COLOR)
        std::ranges::fill(r.fgs(), style.fg);
    if (style.bg != DEFAULT_COLOR)
        std::ranges::fill(r.bgs(), style.bg);
    if (style.em != DEFAULT_EMPHASIS)
        std::ranges::fill(r.ems(), style.em);
}

/// Clear and render one styled line of text.
inline col_t render_line(RasterView & r, std::string text, Style style = {})
{
    clear_line(r, style);
    return render_span(r, Pos::origin(), Span{std::move(text), style});
}

/// Create a one-line layout from a pure assigned-width-to-text function.
template<typename MakeText>
    requires requires(const std::decay_t<MakeText> & make_text, width_t w) {
        { make_text(w) } -> std::convertible_to<std::string>;
    }
inline auto line_text(WidthHint width, MakeText && make_text, Style style = {})
{
    return leaf(
        width,
        HeightHint::fixed(1 * ln),
        [make_text = std::decay_t<MakeText>{std::forward<MakeText>(
             make_text)},
         style](RasterView & r, Size size) {
            render_line(r, make_text(size.w), style);
        });
}

/// Repeat a UTF-8 glyph string `w` terminal cells worth of times.
inline std::string repeat(std::string_view glyph, width_t w)
{
    auto n = w.count();
    std::string result;
    result.reserve(glyph.size() * n);
    for (std::size_t i = 0; i < n; ++i)
        result += glyph;
    return result;
}

/// Display width of UTF-8 text in terminal cells.
inline width_t utf8_width(std::string_view s)
{
    return utf8::display_width(s);
}

/// Create a one-line text leaf using default style.
///
/// Channels at their default (fg/bg = `DEFAULT_COLOR`, em =
/// `DEFAULT_EMPHASIS`) are left untouched on the underlying cells — they
/// inherit whatever the parent painted. Only explicitly-set channels are
/// written.
inline auto text(std::string s)
{
    auto w = utf8_width(s);
    return line_text(
        WidthHint::fixed(w),
        [s = std::move(s)](width_t) { return s; });
}

/// Create a one-line text leaf using `style`.
///
/// Channels at their default in `style` inherit from the parent surface;
/// explicitly-set channels are filled across the leaf's rectangle.
inline auto text(std::string s, Style style)
{
    auto w = utf8_width(s);
    return line_text(
        WidthHint::fixed(w),
        [s = std::move(s)](width_t) { return s; },
        style);
}

/// Create a one-line text leaf that grows to fill its assigned width
/// and truncates with an ellipsis when the assigned width is shorter
/// than the string. Inherit semantics for unset style channels are the
/// same as `text(s, style)`.
inline auto flex_text(std::string s, Style style = {})
{
    return line_text(
        WidthHint::grow(),
        [s = std::move(s)](width_t width) {
            auto w = width.count();
            if (w == 0)
                return std::string{};
            // Byte-truncation: callers asking for stretch behavior are
            // displaying single-line ASCII-ish content (args, headers,
            // status lines). Cluster-aware truncation can come later.
            std::string out = s;
            if (out.size() > w) {
                if (w > 1) {
                    out.resize(w - 1);
                    out += "…";
                } else {
                    out.resize(w);
                }
            }
            return out;
        },
        style);
}

inline auto
styled_lines(std::vector<std::vector<Span>> lines, Style clear = {})
{
    if (lines.empty())
        lines.push_back({});

    auto width = 0 * ch;
    for (const auto & line : lines) {
        auto line_width = 0 * ch;
        for (const auto & span : line)
            line_width += utf8_width(span.text);
        width = std::max(width, line_width);
    }

    auto height = lines.size() * ln;
    return leaf(
        WidthHint::fixed(width),
        HeightHint::fixed(height),
        [lines = std::move(lines), clear](RasterView & r, Size size) {
            std::ranges::fill(r.glyphs(), 32);
            if (clear.fg != DEFAULT_COLOR)
                std::ranges::fill(r.fgs(), clear.fg);
            if (clear.bg != DEFAULT_COLOR)
                std::ranges::fill(r.bgs(), clear.bg);
            if (clear.em != DEFAULT_EMPHASIS)
                std::ranges::fill(r.ems(), clear.em);

            auto row = 0 * ln;
            for (const auto & line : lines) {
                if (row >= size.h)
                    break;

                auto col = Pos::origin().x;
                for (const auto & span : line)
                    col = render_span(
                        r, Pos{col, terminal_origin_v + row}, span);
                row += 1 * ln;
            }
        });
}

inline auto text_lines(std::string s, Style style = {})
{
    std::vector<std::vector<Span>> lines;
    auto current = std::string{};

    auto finish_line = [&] {
        lines.push_back({span(std::move(current), style)});
        current = {};
    };

    for (auto pos = utf8::byte_offset(0); pos.count() < s.size();) {
        auto next = utf8::next(s, pos);
        auto cluster = std::string_view{s}.substr(pos.count(), next - pos);
        if (utf8::is_line_break(cluster)) {
            finish_line();
            pos = next;
            continue;
        }
        current += cluster;
        pos = next;
    }

    if (!current.empty() || lines.empty())
        finish_line();

    return styled_lines(std::move(lines), style);
}

/// Create a compact one-line spinner frame.
inline auto spinner(
    std::size_t tick,
    Style style = bold | fg(Rgba8::black()) | bg(Rgba8::white()))
{
    using namespace std::string_view_literals;
    constexpr auto frames = std::to_array({
        "⠋"sv,
        "⠙"sv,
        "⠹"sv,
        "⠸"sv,
        "⠼"sv,
        "⠴"sv,
        "⠦"sv,
        "⠧"sv,
        "⠇"sv,
        "⠏"sv,
    });

    auto frame = frames[tick % frames.size()];
    return text(" " + std::string{frame} + " ", style);
}

/// Create a one-line text leaf from several styled spans.
template<typename... Spans>
    requires(std::same_as<std::decay_t<Spans>, Span> && ...)
inline auto styled_text(Spans &&... spans)
{
    width_t total_w = 0 * ch;
    ((total_w += utf8_width(spans.text)), ...);

    auto span_tuple = std::tuple{std::forward<Spans>(spans)...};

    return leaf(
        WidthHint::fixed(total_w),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size) {
            col_t col = Pos::origin().x;
            std::apply(
                [&](const auto &... s) {
                    ((col = render_span(r, Pos{col, Pos::origin().y}, s)),
                     ...);
                },
                span_tuple);
        });
}

/// Fill available space with a background color.
inline auto fill(Rgba8 color = Rgba8(60, 60, 60))
{
    return leaf(
        WidthHint::grow(), HeightHint::grow(), [=](RasterView & r, Size) {
            std::ranges::fill(r.bgs(), color);
        });
}

/// One-line bg-colored strip of `width` cells. Useful as a leading or
/// trailing pad inside a row.
inline auto hfill(width_t width, Rgba8 color)
{
    return leaf(
        WidthHint::fixed(width),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size) {
            std::ranges::fill(r.glyphs(), 32);
            std::ranges::fill(r.bgs(), color);
        });
}

/// One-line bg-colored strip that grows to fill remaining horizontal
/// space. Useful as the trailing element of a row that should read as a
/// continuous band.
inline auto flex_fill(Rgba8 color)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size) {
            std::ranges::fill(r.glyphs(), 32);
            std::ranges::fill(r.bgs(), color);
        });
}

/// Build a horizontal rule string for a width.
inline std::string hrule_string(width_t w)
{
    return repeat("─", w);
}

/// Create a one-line horizontal rule layout.
inline auto hrule()
{
    return line_text(
        WidthHint::grow(),
        [](width_t width) { return hrule_string(width); });
}

/// Build the glyph string for a fractional progress bar.
inline std::string bar_string(percent_t pct, width_t width)
{
    auto fraction = std::clamp(pct.value(), 0.0, 100.0) / 100.0;
    return chart::progress_bar(fraction, width.count());
}

/// Build the glyph string for a fractional range progress bar.
inline std::string range_bar_string(
    double begin,
    double end,
    width_t width)
{
    return chart::range_bar(begin, end, width.count());
}

/// Create a one-line progress bar layout.
inline auto progress_bar(
    percent_t pct,
    Rgba8 fg = Rgba8(100, 180, 255),
    Rgba8 bg = Rgba8(50, 50, 50))
{
    return line_text(
        WidthHint::grow(),
        [=](width_t width) { return bar_string(pct, width); },
        Style{fg, bg, DEFAULT_EMPHASIS});
}

/// Create a one-line progress bar for a subrange of [0,1].
inline auto range_progress_bar(
    double begin,
    double end,
    Rgba8 fg = Rgba8(100, 180, 255),
    Rgba8 bg = Rgba8(50, 50, 50))
{
    return line_text(
        WidthHint::grow(),
        [=](width_t width) {
            return range_bar_string(begin, end, width);
        },
        Style{fg, bg, DEFAULT_EMPHASIS});
}

/// Horizontal flex container.
template<Layout... Children>
struct Row
{
    /// Child layouts arranged left to right.
    std::tuple<Children...> children;

    /// Sum child minimum widths and flex factors.
    constexpr WidthHint width_hint() const
    {
        width_t total_min = 0 * ch;
        ratio_t total_flex = 0.0 * one;
        std::apply(
            [&](const auto &... c) {
                ((total_min += c.width_hint().min,
                  total_flex += c.width_hint().flex),
                 ...);
            },
            children);
        return {total_min, total_flex};
    }

    /// Use the tallest child minimum height.
    constexpr HeightHint height_hint() const
    {
        height_t max_min = 0 * ln;
        std::apply(
            [&](const auto &... c) {
                ((max_min = std::max(max_min, c.height_hint().min)), ...);
            },
            children);
        return HeightHint::fixed(
            max_min.count() > 0 ? max_min : height_t{1 * ln});
    }

    /// Divide width among children and render them left to right.
    void render(RasterView & raster, Size size) const
    {
        constexpr std::size_t N = sizeof...(Children);
        if constexpr (N == 0)
            return;

        std::array<WidthHint, N> hints;
        std::size_t i = 0;
        std::apply(
            [&](const auto &... c) {
                ((hints[i++] = c.width_hint()), ...);
            },
            children);

        auto widths = flex_distribute(size.w, hints);

        Pos cursor = Pos::origin();
        i = 0;
        std::apply(
            [&](const auto &... c) {
                (
                    [&] {
                        auto child_size = Size{widths[i], size.h};
                        if (widths[i].count() > 0) {
                            auto sub =
                                subraster(raster, cursor, child_size);
                            c.render(sub, child_size);
                            cursor += widths[i];
                        }
                        ++i;
                    }(),
                    ...);
            },
            children);
    }

private:
    template<std::size_t N>
    static std::array<width_t, N>
    flex_distribute(width_t total, const std::array<WidthHint, N> & hints)
    {
        std::array<width_t, N> result{};

        width_t used = 0 * ch;
        ratio_t total_flex = 0.0 * one;
        for (std::size_t i = 0; i < N; ++i) {
            result[i] = hints[i].min;
            used += hints[i].min;
            total_flex += hints[i].flex;
        }

        if (total_flex > 0 && total > used) {
            auto remaining = total - used;
            for (std::size_t i = 0; i < N; ++i) {
                auto flex_val = hints[i].flex;
                auto total_flex_val = total_flex;
                if (flex_val > 0)
                    result[i] +=
                        remaining
                        * (flex_val.value() / total_flex_val.value());
            }
        }

        return result;
    }
};

/// Create a horizontal flex row.
template<Layout... Children>
Row<std::decay_t<Children>...> row(Children &&... children)
{
    return {std::tuple{std::forward<Children>(children)...}};
}

/// Vertical flex container.
template<Layout... Children>
struct Column
{
    /// Child layouts arranged top to bottom.
    std::tuple<Children...> children;

    /// Use the widest child minimum width and grow horizontally.
    constexpr WidthHint width_hint() const
    {
        width_t max_min = 0 * ch;
        std::apply(
            [&](const auto &... c) {
                ((max_min = std::max(max_min, c.width_hint().min)), ...);
            },
            children);
        return {max_min, 1.0 * one};
    }

    /// Sum child minimum heights and flex factors.
    constexpr HeightHint height_hint() const
    {
        height_t total_min = 0 * ln;
        ratio_t total_flex = 0.0 * one;
        std::apply(
            [&](const auto &... c) {
                ((total_min += c.height_hint().min,
                  total_flex += c.height_hint().flex),
                 ...);
            },
            children);
        return {total_min, total_flex};
    }

    /// Divide height among children and render them top to bottom.
    void render(RasterView & raster, Size size) const
    {
        constexpr std::size_t N = sizeof...(Children);
        if constexpr (N == 0)
            return;

        std::array<HeightHint, N> hints;
        std::size_t i = 0;
        std::apply(
            [&](const auto &... c) {
                ((hints[i++] = c.height_hint()), ...);
            },
            children);

        auto heights = flex_distribute(size.h, hints);

        Pos cursor = Pos::origin();
        i = 0;
        std::apply(
            [&](const auto &... c) {
                (
                    [&] {
                        auto child_size = Size{size.w, heights[i]};
                        if (heights[i].count() > 0) {
                            auto sub =
                                subraster(raster, cursor, child_size);
                            c.render(sub, child_size);
                            cursor = cursor
                                     + heights[i]; // point + vector = point
                        }
                        ++i;
                    }(),
                    ...);
            },
            children);
    }

private:
    template<std::size_t N>
    static std::array<height_t, N>
    flex_distribute(height_t total, const std::array<HeightHint, N> & hints)
    {
        std::array<height_t, N> result{};

        height_t used = 0 * ln;
        ratio_t total_flex = 0.0 * one;
        for (std::size_t i = 0; i < N; ++i) {
            result[i] = hints[i].min;
            used += hints[i].min;
            total_flex += hints[i].flex;
        }

        if (total_flex > 0 && total > used) {
            auto remaining = total - used;
            for (std::size_t i = 0; i < N; ++i) {
                auto flex_val = hints[i].flex;
                auto total_flex_val = total_flex;
                if (flex_val > 0)
                    result[i] +=
                        remaining
                        * (flex_val.value() / total_flex_val.value());
            }
        }

        return result;
    }
};

/// Create a vertical flex column.
template<Layout... Children>
Column<std::decay_t<Children>...> column(Children &&... children)
{
    return {std::tuple{std::forward<Children>(children)...}};
}

/// Dynamic horizontal flex container for a runtime-sized vector of
/// children that all have the same concrete layout type.
template<Layout Child>
struct DynRow
{
    std::vector<Child> children;

    WidthHint width_hint() const
    {
        width_t total_min = 0 * ch;
        ratio_t total_flex = 0.0 * one;
        for (const auto & c : children) {
            total_min += c.width_hint().min;
            total_flex += c.width_hint().flex;
        }
        return {total_min, total_flex};
    }

    HeightHint height_hint() const
    {
        height_t max_min = 0 * ln;
        for (const auto & c : children)
            max_min = std::max(max_min, c.height_hint().min);
        return HeightHint::fixed(
            max_min.count() > 0 ? max_min : height_t{1 * ln});
    }

    void render(RasterView & raster, Size size) const
    {
        width_t used = 0 * ch;
        ratio_t total_flex = 0.0 * one;
        for (const auto & c : children) {
            used += c.width_hint().min;
            total_flex += c.width_hint().flex;
        }

        Pos cursor = Pos::origin();
        for (const auto & c : children) {
            auto h = c.width_hint();
            auto w = h.min;
            if (h.flex > 0.0 * one && size.w > used)
                w += (size.w - used)
                     * (h.flex.value() / total_flex.value());
            if (w.count() > 0) {
                auto child_size = Size{w, size.h};
                auto sub = subraster(raster, cursor, child_size);
                c.render(sub, child_size);
                cursor += w;
            }
        }
    }
};

template<Layout Child>
auto dyn_row(std::vector<Child> children)
{
    return DynRow<Child>{std::move(children)};
}

/// Dynamic vertical flex container for a runtime-sized vector of
/// children that all have the same concrete layout type.
template<Layout Child>
struct DynColumn
{
    std::vector<Child> children;

    WidthHint width_hint() const
    {
        width_t max_min = 0 * ch;
        for (const auto & c : children)
            max_min = std::max(max_min, c.width_hint().min);
        return {max_min, 1.0 * one};
    }

    HeightHint height_hint() const
    {
        height_t total_min = 0 * ln;
        for (const auto & c : children)
            total_min += c.height_hint().min;
        return HeightHint::fixed(total_min);
    }

    void render(RasterView & raster, Size size) const
    {
        Pos cursor = Pos::origin();
        for (const auto & c : children) {
            auto h = c.height_hint().min;
            if (h.count() == 0)
                continue;
            if ((cursor.y - Pos::origin().y) + h > size.h)
                break;
            auto child_size = Size{size.w, h};
            auto sub = subraster(raster, cursor, child_size);
            c.render(sub, child_size);
            cursor = cursor + h;
        }
    }
};

template<Layout Child>
auto dyn_column(std::vector<Child> children)
{
    return DynColumn<Child>{std::move(children)};
}

/// Render a span of items by mapping each item to a one-line layout.
template<typename T, typename ViewFn>
struct List
{
    /// Borrowed items to render.
    std::span<const T> items;
    /// Function that turns an item into a layout.
    ViewFn view;

    /// Lists grow to fill available width.
    constexpr WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    /// One line per item.
    constexpr HeightHint height_hint() const
    {
        return HeightHint::fixed(items.size() * ln);
    }

    /// Render visible items until the assigned height is filled.
    void render(RasterView & raster, Size size) const
    {
        const auto row_size = Size{size.w, 1 * ln};

        Pos cursor = Pos::origin();

        for (const auto & item : items) {
            if (cursor.y - Pos::origin().y >= size.h)
                break;

            auto child = view(item);
            auto sub = subraster(raster, cursor, row_size);

            child.render(sub, row_size);
            cursor += 1 * ln;
        }
    }
};

/// Create a list from a borrowed item span.
template<typename T, typename ViewFn>
List<T, ViewFn> list(std::span<const T> items, ViewFn && view)
{
    return {items, std::forward<ViewFn>(view)};
}

/// Create a list from a vector, borrowing it for the lifetime of the
/// layout.
template<typename T, typename ViewFn>
auto list(const std::vector<T> & items, ViewFn && view)
{
    return list(std::span<const T>(items), std::forward<ViewFn>(view));
}

} // namespace nxt::tui
