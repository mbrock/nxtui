#pragma once

#include "nxtui/any_layout.hpp"
#include "nxtui/raster.hpp"
#include "nxtui/chart.hpp"
#include "nxtui/layout.hpp"
#include "nxtui/units.hpp"
#include "nxtui/utf8.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace nxtui::tui {

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

/// Layout that claims no space and renders nothing.
struct Empty
{
    constexpr WidthHint width_hint() const
    {
        return {};
    }

    constexpr HeightHint height_hint() const
    {
        return {};
    }

    void render(RasterView &, Size) const {}
};

/// Empty layout used when a typed composition needs an absent child.
constexpr Empty empty()
{
    return {};
}

/// Layout that is exactly one of several statically known alternatives.
///
/// The alternative is chosen when the layout is built; hints and rendering
/// come from the chosen one only.
template<Layout... Alternatives>
struct OneOf
{
    std::variant<Alternatives...> chosen;

    /// Build the layout showing alternative `I`.
    template<std::size_t I, typename... Args>
    static constexpr OneOf pick(Args &&... args)
    {
        return {decltype(chosen)(
            std::in_place_index<I>, std::forward<Args>(args)...)};
    }

    constexpr WidthHint width_hint() const
    {
        return std::visit(
            [](const auto & l) -> WidthHint { return l.width_hint(); },
            chosen);
    }

    constexpr HeightHint height_hint() const
    {
        return std::visit(
            [](const auto & l) -> HeightHint { return l.height_hint(); },
            chosen);
    }

    void render(RasterView & raster, Size size) const
    {
        std::visit([&](const auto & l) { l.render(raster, size); }, chosen);
    }
};

/// Conditional layout over a false and a true alternative.
template<Layout FalseLayout, Layout TrueLayout>
using Either = OneOf<FalseLayout, TrueLayout>;

/// Create a conditional layout from two alternatives.
template<Layout FalseLayout, Layout TrueLayout>
constexpr auto either(
    bool choose_true,
    FalseLayout && false_layout,
    TrueLayout && true_layout)
{
    using Result = Either<std::decay_t<FalseLayout>, std::decay_t<TrueLayout>>;
    if (choose_true)
        return Result::template pick<1>(std::forward<TrueLayout>(true_layout));
    return Result::template pick<0>(std::forward<FalseLayout>(false_layout));
}

/// Nullary callable that builds a layout on demand.
template<typename F>
concept LayoutThunk =
    std::invocable<F &> && Layout<std::invoke_result_t<F &>>;

/// Create a conditional layout, building only the chosen alternative.
template<LayoutThunk MakeFalse, LayoutThunk MakeTrue>
constexpr auto either(bool choose_true, MakeFalse make_false, MakeTrue make_true)
{
    using Result = Either<
        std::invoke_result_t<MakeFalse &>,
        std::invoke_result_t<MakeTrue &>>;
    if (choose_true)
        return Result::template pick<1>(make_true());
    return Result::template pick<0>(make_false());
}

/// Conditional layout that renders a child only when `condition` is true.
template<Layout Child>
constexpr auto when(bool condition, Child && child)
{
    return either(condition, empty(), std::forward<Child>(child));
}

/// Conditional layout that builds and renders a child only when
/// `condition` is true.
template<LayoutThunk MakeChild>
constexpr auto when(bool condition, MakeChild make_child)
{
    return either(condition, [] { return empty(); }, std::move(make_child));
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
constexpr auto surface(Style style, Child && child)
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
constexpr auto fixed_height(height_t height, Child && child)
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
constexpr auto fixed_width(width_t width, Child && child)
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
constexpr auto grow_width(Child && child, ratio_t factor = 1.0 * one)
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

/// Direction in which a `Stack` places its children.
enum class Axis { row, column };

/// A runtime-sized sequence of layouts of one type, such as
/// `std::vector<AnyLayout>` or a span of concrete child layouts.
template<typename R>
concept LayoutRange =
    std::ranges::forward_range<const R>
    && Layout<std::ranges::range_value_t<const R>>;

/// Call `f` on each child of a statically shaped child tuple.
template<typename... Children, typename F>
constexpr void for_each_child(const std::tuple<Children...> & children, F && f)
{
    std::apply([&](const auto &... child) { (f(child), ...); }, children);
}

/// Call `f` on each child of a runtime-sized child range.
template<LayoutRange Children, typename F>
constexpr void for_each_child(const Children & children, F && f)
{
    for (const auto & child : children)
        f(child);
}

template<Axis A>
struct axis_traits;

template<>
struct axis_traits<Axis::row>
{
    using main_hint = WidthHint;
    using cross_extent = height_t;

    static constexpr WidthHint main(const Layout auto & l)
    {
        return l.width_hint();
    }

    static constexpr HeightHint cross(const Layout auto & l)
    {
        return l.height_hint();
    }

    static constexpr width_t extent(Size size)
    {
        return size.w;
    }

    static constexpr Size child_size(Size size, width_t w)
    {
        return {w, size.h};
    }

    /// Rows are as tall as their tallest child, and at least one line.
    static constexpr HeightHint cross_hint(height_t tallest)
    {
        return HeightHint::fixed(
            tallest.count() > 0 ? tallest : height_t{1 * ln});
    }
};

template<>
struct axis_traits<Axis::column>
{
    using main_hint = HeightHint;
    using cross_extent = width_t;

    static constexpr HeightHint main(const Layout auto & l)
    {
        return l.height_hint();
    }

    static constexpr WidthHint cross(const Layout auto & l)
    {
        return l.width_hint();
    }

    static constexpr height_t extent(Size size)
    {
        return size.h;
    }

    static constexpr Size child_size(Size size, height_t h)
    {
        return {size.w, h};
    }

    /// Columns are at least as wide as their widest child, and grow.
    static constexpr WidthHint cross_hint(width_t widest)
    {
        return {widest, 1.0 * one};
    }
};

/// Flex container placing `Children` one after another along axis `A`.
///
/// `Children` is either a `std::tuple` of layouts, for compositions whose
/// shape is known statically, or a `LayoutRange` for runtime-sized ones.
/// Along the main axis, children get their minimum extent plus a share of
/// the leftover space proportional to their flex factor.
template<Axis A, typename Children>
struct Stack
{
    using axis = axis_traits<A>;

    /// Child layouts in placement order.
    Children children;

    /// Sum of child minimum extents and flex factors along the main axis.
    constexpr auto main_hint() const
    {
        auto total = typename axis::main_hint{};
        for_each_child(children, [&](const auto & child) {
            auto hint = axis::main(child);
            total.min += hint.min;
            total.flex += hint.flex;
        });
        return total;
    }

    /// Largest child minimum extent across the main axis.
    constexpr auto cross_hint() const
    {
        auto largest = typename axis::cross_extent{};
        for_each_child(children, [&](const auto & child) {
            largest = std::max(largest, axis::cross(child).min);
        });
        return axis::cross_hint(largest);
    }

    constexpr WidthHint width_hint() const
    {
        if constexpr (A == Axis::row)
            return main_hint();
        else
            return cross_hint();
    }

    constexpr HeightHint height_hint() const
    {
        if constexpr (A == Axis::column)
            return main_hint();
        else
            return cross_hint();
    }

    /// Divide the main axis among children and render them in order.
    void render(RasterView & raster, Size size) const
    {
        auto total = main_hint();
        auto available = axis::extent(size);
        auto leftover = available > total.min ? available - total.min
                                               : decltype(available){};

        Pos cursor = Pos::origin();
        for_each_child(children, [&](const auto & child) {
            auto hint = axis::main(child);
            auto extent = hint.min;
            if (hint.flex > 0 && total.flex > 0 && leftover.count() > 0)
                extent += leftover * (hint.flex.value() / total.flex.value());
            if (extent.count() == 0)
                return;
            auto child_size = axis::child_size(size, extent);
            auto sub = subraster(raster, cursor, child_size);
            child.render(sub, child_size);
            cursor += extent;
        });
    }
};

/// Horizontal flex container with a statically known set of children.
template<Layout... Children>
using Row = Stack<Axis::row, std::tuple<Children...>>;

/// Vertical flex container with a statically known set of children.
template<Layout... Children>
using Column = Stack<Axis::column, std::tuple<Children...>>;

/// Create a horizontal flex row.
template<Layout... Children>
constexpr Row<std::decay_t<Children>...> row(Children &&... children)
{
    return {std::tuple<std::decay_t<Children>...>{
        std::forward<Children>(children)...}};
}

/// Create a horizontal flex row from a runtime-sized range of children.
template<LayoutRange Children>
constexpr auto row(Children && children)
{
    return Stack<Axis::row, std::decay_t<Children>>{
        std::forward<Children>(children)};
}

/// Create a vertical flex column.
template<Layout... Children>
constexpr Column<std::decay_t<Children>...> column(Children &&... children)
{
    return {std::tuple<std::decay_t<Children>...>{
        std::forward<Children>(children)...}};
}

/// Create a vertical flex column from a runtime-sized range of children.
template<LayoutRange Children>
constexpr auto column(Children && children)
{
    return Stack<Axis::column, std::decay_t<Children>>{
        std::forward<Children>(children)};
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

} // namespace nxtui::tui
