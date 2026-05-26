#pragma once

#include "nxt/any_layout.hpp"
#include "nxt/raster.hpp"
#include "nxt/chart.hpp"
#include "nxt/layout.hpp"
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

/// Create a conditional layout from two alternatives.
template<Layout FalseLayout, Layout TrueLayout>
AnyLayout either(
    bool choose_true,
    FalseLayout && false_layout,
    TrueLayout && true_layout)
{
    if (choose_true)
        return AnyLayout{std::forward<TrueLayout>(true_layout)};
    return AnyLayout{std::forward<FalseLayout>(false_layout)};
}

/// Conditional layout that renders a child only when `condition` is true.
template<Layout Child>
AnyLayout when(bool condition, Child && child)
{
    if (condition)
        return AnyLayout{std::forward<Child>(child)};
    return AnyLayout{empty()};
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
struct SurfaceLayout
{
    /// Style used for every cell in the clear pass.
    Style style{};
    /// Child rendered after the clear pass.
    AnyLayout child;

    /// Forward the child's width hint.
    WidthHint width_hint() const
    {
        return child.width_hint();
    }

    /// Forward the child's height hint.
    HeightHint height_hint() const
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
AnyLayout surface(Style style, Child && child)
{
    return AnyLayout{SurfaceLayout{
        style,
        AnyLayout{std::forward<Child>(child)},
    }};
}

/// Layout decorator that forces a fixed height hint.
struct FixedHeightLayout
{
    /// Height reported to parent columns and HUD sizing.
    height_t height{0 * ln};
    /// Child rendered with whatever size the parent assigns.
    AnyLayout child;

    /// Forward the child's width hint.
    WidthHint width_hint() const
    {
        return child.width_hint();
    }

    /// Return the fixed height hint.
    HeightHint height_hint() const
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
AnyLayout fixed_height(height_t height, Child && child)
{
    return AnyLayout{FixedHeightLayout{
        height,
        AnyLayout{std::forward<Child>(child)},
    }};
}

/// Layout decorator that forces a fixed width hint.
struct FixedWidthLayout
{
    /// Width reported to parent rows and HUD sizing.
    width_t width{0 * ch};
    /// Child rendered with whatever size the parent assigns.
    AnyLayout child;

    /// Return the fixed width hint.
    WidthHint width_hint() const
    {
        return WidthHint::fixed(width);
    }

    /// Forward the child's height hint.
    HeightHint height_hint() const
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
AnyLayout fixed_width(width_t width, Child && child)
{
    return AnyLayout{FixedWidthLayout{
        width,
        AnyLayout{std::forward<Child>(child)},
    }};
}

/// Layout decorator that lets a child claim remaining row width.
struct GrowWidthLayout
{
    AnyLayout child;
    ratio_t factor{1.0 * one};

    WidthHint width_hint() const
    {
        auto hint = child.width_hint();
        hint.flex = std::max(hint.flex, factor);
        return hint;
    }

    HeightHint height_hint() const
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
AnyLayout grow_width(Child && child, ratio_t factor = 1.0 * one)
{
    return AnyLayout{GrowWidthLayout{
        AnyLayout{std::forward<Child>(child)},
        factor,
    }};
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
struct RowLayout
{
    /// Child layouts arranged left to right.
    std::vector<AnyLayout> children;

    /// Sum child minimum widths and flex factors.
    WidthHint width_hint() const
    {
        width_t total_min = 0 * ch;
        ratio_t total_flex = 0.0 * one;
        for (const auto & child : children) {
            auto hint = child.width_hint();
            total_min += hint.min;
            total_flex += hint.flex;
        }
        return {total_min, total_flex};
    }

    /// Use the tallest child minimum height.
    HeightHint height_hint() const
    {
        height_t max_min = 0 * ln;
        for (const auto & child : children)
            max_min = std::max(max_min, child.height_hint().min);
        return HeightHint::fixed(
            max_min.count() > 0 ? max_min : height_t{1 * ln});
    }

    /// Divide width among children and render them left to right.
    void render(RasterView & raster, Size size) const
    {
        if (children.empty())
            return;

        auto hints = std::vector<WidthHint>{};
        hints.reserve(children.size());
        for (const auto & child : children)
            hints.push_back(child.width_hint());

        auto widths = flex_distribute(size.w, hints);

        Pos cursor = Pos::origin();
        for (std::size_t i = 0; i < children.size(); ++i) {
            auto child_size = Size{widths[i], size.h};
            if (widths[i].count() > 0) {
                auto sub = subraster(raster, cursor, child_size);
                children[i].render(sub, child_size);
                cursor += widths[i];
            }
        }
    }

private:
    static std::vector<width_t>
    flex_distribute(width_t total, const std::vector<WidthHint> & hints)
    {
        auto result = std::vector<width_t>(hints.size());

        width_t used = 0 * ch;
        ratio_t total_flex = 0.0 * one;
        for (std::size_t i = 0; i < hints.size(); ++i) {
            result[i] = hints[i].min;
            used += hints[i].min;
            total_flex += hints[i].flex;
        }

        if (total_flex > 0 && total > used) {
            auto remaining = total - used;
            for (std::size_t i = 0; i < hints.size(); ++i) {
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

inline RowLayout row(std::vector<AnyLayout> children)
{
    return RowLayout{std::move(children)};
}

/// Create a horizontal flex row.
template<Layout... Children>
RowLayout row(Children &&... children)
{
    auto layouts = std::vector<AnyLayout>{};
    layouts.reserve(sizeof...(Children));
    (layouts.emplace_back(std::forward<Children>(children)), ...);
    return row(std::move(layouts));
}

/// Vertical flex container.
struct ColumnLayout
{
    /// Child layouts arranged top to bottom.
    std::vector<AnyLayout> children;

    /// Use the widest child minimum width and grow horizontally.
    WidthHint width_hint() const
    {
        width_t max_min = 0 * ch;
        for (const auto & child : children)
            max_min = std::max(max_min, child.width_hint().min);
        return {max_min, 1.0 * one};
    }

    /// Sum child minimum heights and flex factors.
    HeightHint height_hint() const
    {
        height_t total_min = 0 * ln;
        ratio_t total_flex = 0.0 * one;
        for (const auto & child : children) {
            auto hint = child.height_hint();
            total_min += hint.min;
            total_flex += hint.flex;
        }
        return {total_min, total_flex};
    }

    /// Divide height among children and render them top to bottom.
    void render(RasterView & raster, Size size) const
    {
        if (children.empty())
            return;

        auto hints = std::vector<HeightHint>{};
        hints.reserve(children.size());
        for (const auto & child : children)
            hints.push_back(child.height_hint());

        auto heights = flex_distribute(size.h, hints);

        Pos cursor = Pos::origin();
        for (std::size_t i = 0; i < children.size(); ++i) {
            auto child_size = Size{size.w, heights[i]};
            if (heights[i].count() > 0) {
                auto sub = subraster(raster, cursor, child_size);
                children[i].render(sub, child_size);
                cursor = cursor + heights[i];
            }
        }
    }

private:
    static std::vector<height_t>
    flex_distribute(height_t total, const std::vector<HeightHint> & hints)
    {
        auto result = std::vector<height_t>(hints.size());

        height_t used = 0 * ln;
        ratio_t total_flex = 0.0 * one;
        for (std::size_t i = 0; i < hints.size(); ++i) {
            result[i] = hints[i].min;
            used += hints[i].min;
            total_flex += hints[i].flex;
        }

        if (total_flex > 0 && total > used) {
            auto remaining = total - used;
            for (std::size_t i = 0; i < hints.size(); ++i) {
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

inline ColumnLayout column(std::vector<AnyLayout> children)
{
    return ColumnLayout{std::move(children)};
}

/// Create a vertical flex column.
template<Layout... Children>
ColumnLayout column(Children &&... children)
{
    auto layouts = std::vector<AnyLayout>{};
    layouts.reserve(sizeof...(Children));
    (layouts.emplace_back(std::forward<Children>(children)), ...);
    return column(std::move(layouts));
}

/// Dynamic horizontal flex container for a runtime-sized vector of
/// children. Kept as a compatibility alias for callers that already
/// materialize same-typed children.
template<Layout Child>
RowLayout dyn_row(std::vector<Child> children)
{
    if constexpr (std::same_as<Child, AnyLayout>) {
        return row(std::move(children));
    } else {
        auto layouts = std::vector<AnyLayout>{};
        layouts.reserve(children.size());
        for (auto & child : children)
            layouts.emplace_back(std::move(child));
        return row(std::move(layouts));
    }
}

/// Dynamic vertical flex container for a runtime-sized vector of
/// children. Kept as a compatibility alias for callers that already
/// materialize same-typed children.
template<Layout Child>
ColumnLayout dyn_column(std::vector<Child> children)
{
    if constexpr (std::same_as<Child, AnyLayout>) {
        return column(std::move(children));
    } else {
        auto layouts = std::vector<AnyLayout>{};
        layouts.reserve(children.size());
        for (auto & child : children)
            layouts.emplace_back(std::move(child));
        return column(std::move(layouts));
    }
}

/// Dynamic vertical container for a borrowed runtime-sized span of data.
///
/// Each item is mapped to a concrete layout when measured or rendered. This
/// is the multi-line counterpart to `list`: item layouts may have arbitrary
/// heights, and no vector of materialized child layouts is retained.
template<typename T, typename ViewFn>
struct Each
{
    std::span<const T> items;
    ViewFn view;

    WidthHint width_hint() const
    {
        width_t max_min = 0 * ch;
        for (const auto & item : items) {
            auto child = view(item);
            max_min = std::max(max_min, child.width_hint().min);
        }
        return {max_min, 1.0 * one};
    }

    HeightHint height_hint() const
    {
        height_t total_min = 0 * ln;
        for (const auto & item : items) {
            auto child = view(item);
            total_min += child.height_hint().min;
        }
        return HeightHint::fixed(total_min);
    }

    void render(RasterView & raster, Size size) const
    {
        Pos cursor = Pos::origin();
        for (const auto & item : items) {
            auto child = view(item);
            auto h = child.height_hint().min;
            if (h.count() == 0)
                continue;
            if ((cursor.y - Pos::origin().y) + h > size.h)
                break;
            auto child_size = Size{size.w, h};
            auto sub = subraster(raster, cursor, child_size);
            child.render(sub, child_size);
            cursor = cursor + h;
        }
    }
};

template<typename T, typename ViewFn>
auto each(std::span<const T> items, ViewFn && view)
{
    return Each<T, std::decay_t<ViewFn>>{
        items,
        std::forward<ViewFn>(view)};
}

template<typename T, typename ViewFn>
auto each(const std::vector<T> & items, ViewFn && view)
{
    return each(std::span<const T>{items}, std::forward<ViewFn>(view));
}

/// Owning variant used by value-returning helper functions that compute the
/// data locally and package it into a layout.
template<typename T, typename ViewFn>
struct OwningEach
{
    std::vector<T> items;
    ViewFn view;

    WidthHint width_hint() const
    {
        return each(items, view).width_hint();
    }

    HeightHint height_hint() const
    {
        return each(items, view).height_hint();
    }

    void render(RasterView & raster, Size size) const
    {
        each(items, view).render(raster, size);
    }
};

template<typename T, typename ViewFn>
auto each(std::vector<T> && items, ViewFn && view)
{
    return OwningEach<T, std::decay_t<ViewFn>>{
        std::move(items),
        std::forward<ViewFn>(view)};
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
