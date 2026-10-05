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

/**
 * @namespace nxtui::tui
 * Composable layout values: build a tree of small value types for each
 * frame, measure it, and render it into a `RasterView`.
 *
 * Every layout satisfies the `Layout` concept: it reports a `WidthHint` and
 * `HeightHint` (minimum extent plus flex factor) and renders into whatever
 * `Size` its parent assigns. Leaves draw content: `text`, `styled_text`,
 * `flex_text`, `text_lines`, `progress_bar`, `hrule`, `fill`, `spinner`,
 * `sparkline`, `text_field`, `vterm_screen`, or any callback via `leaf`.
 * Containers place children: `row` and `column` (flex stacks), `each` and
 * `list` (one child per data item). Decorators adjust one child:
 * `surface`, `fixed_width`, `fixed_height`, `grow_width`. Conditionals
 * (`either`, `when`, `OneOf`) pick between statically typed alternatives.
 *
 * Compositions keep their concrete types, so a frame is one stack value
 * with no allocation for the tree itself. Use `AnyLayout` where the shape
 * really varies at runtime, and `Slot` for a cell another task updates.
 *
 * Styles compose with `|`: `fg(Rgba8::red()) | bold`. A channel left at its
 * default (`DEFAULT_COLOR`, `Emphasis::none`) is not written, so the cell
 * keeps what an enclosing `surface` or earlier write put there.
 *
 * @code
 * auto layout = nxtui::tui::column(
 *     nxtui::tui::text("build", nxtui::tui::bold),
 *     nxtui::tui::progress_bar(42 * nxtui::percent),
 *     nxtui::tui::hrule());
 * auto & buffer = compositor.back_buffer();
 * buffer.clear();
 * auto view = buffer.view();
 * layout.render(view, buffer.extent());
 * compositor.present_frame(std::cout);
 * @endcode
 */
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

/// Leaf with fixed hints `w` and `h` that renders by calling
/// `f(RasterView &, Size)`.
///
/// `f` is stored by value. If it captures references, the referents must
/// outlive every render of the leaf.
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
    /// The alternative being shown.
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

/// Show `true_layout` if `choose_true`, else `false_layout`. Both are built
/// eagerly; the result type is the same either way.
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

/// Free-function form of `RasterView::write_text`.
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

/// Layout decorator that paints its whole raster with `style` and blank
/// glyphs, then renders the child on top.
///
/// Unlike most leaves it writes every channel, including ones at their
/// defaults, so it gives children a known background to inherit.
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
    /// Wrapped layout.
    Child child;
    /// Minimum flex factor reported for the child.
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

/// Keep the child's minimum width but raise its width flex to at least
/// `factor`, so it takes a share of leftover row width.
template<Layout Child>
constexpr auto grow_width(Child && child, ratio_t factor = 1.0 * one)
{
    return GrowWidth<std::decay_t<Child>>{
        std::forward<Child>(child), factor};
}

/// Write one styled span at `pos` and return the column after it. Only the
/// style channels that are set are written, and only on the cells the
/// text covers.
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

/// Clear `r` with `clear_line(r, style)` and write `text` at its origin.
inline col_t render_line(RasterView & r, std::string text, Style style = {})
{
    clear_line(r, style);
    return render_span(r, Pos::origin(), Span{std::move(text), style});
}

/// One-line leaf whose text is computed from the width it is given.
///
/// `make_text(width)` runs on every render; the line is cleared (see
/// `clear_line`) and the result written from column 0. The `width`
/// parameter is the leaf's width hint.
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

/// Display width of UTF-8 text in terminal cells (see
/// `utf8::display_width`).
inline width_t utf8_width(std::string_view s)
{
    return utf8::display_width(s);
}

/// One-line leaf showing `s`, with a fixed width equal to its display
/// width.
///
/// Colors and emphasis are left untouched, so the text inherits whatever
/// the parent painted. The text should not contain line breaks; use
/// `text_lines` for multi-line text.
inline auto text(std::string s)
{
    auto w = utf8_width(s);
    return line_text(
        WidthHint::fixed(w),
        [s = std::move(s)](width_t) { return s; });
}

/// One-line leaf showing `s` in `style`.
///
/// Channels set in `style` are filled across the leaf's whole assigned
/// rectangle; channels left at their default inherit from the parent.
inline auto text(std::string s, Style style)
{
    auto w = utf8_width(s);
    return line_text(
        WidthHint::fixed(w),
        [s = std::move(s)](width_t) { return s; },
        style);
}

/// One-line leaf that grows to fill its assigned width and truncates with
/// an ellipsis when the text is longer. Style channels behave as in
/// `text(s, style)`.
///
/// Truncation counts bytes, not cells, so it is exact only for
/// single-width ASCII and can cut a multi-byte character.
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

/// Multi-line leaf: one line per inner vector of spans.
///
/// Fixed width is the widest line; fixed height is the number of lines (at
/// least one). Rendering blanks the glyphs, fills the channels set in
/// `clear`, then writes each line's spans from column 0, stopping at the
/// assigned height.
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

/// Multi-line leaf splitting `s` at line breaks (`\n`, `\r\n`, `\r`),
/// each line in `style`. Lines are not wrapped; see
/// `text_flow::markdown_block` for wrapping.
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

/// One-line, three-cell braille spinner showing frame `tick % 10`.
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

/// One-line leaf writing several styled spans left to right; fixed width
/// is their total display width.
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

/// Leaf that grows in both directions and sets the background of its
/// area to `color`, leaving glyphs as they are.
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

/// One-line `─` rule that grows to the assigned width.
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

/// One-line bar that grows to the assigned width and fills `pct` of it
/// (clamped to 0-100%) with eighth-cell block glyphs in `fg` over `bg`.
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

/// One-line bar like `progress_bar` that fills only the fraction range
/// [`begin`, `end`] of the width (both clamped to [0, 1]).
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
/// Usually built with `row` or `column`.
///
/// Measuring: the main-axis hint sums the children's minimums and flex
/// factors. Across the axis a row is a fixed height equal to its tallest
/// child's minimum (at least one line); a column is at least as wide as its
/// widest child and always grows.
///
/// Rendering: each child gets its minimum plus a share of the leftover
/// space proportional to its flex (rounded down), and the full cross
/// extent. Children are not shrunk when space is short; later ones are
/// clipped instead. Children whose share is zero are skipped.
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

        Pos cursor = Pos::origin();
        for_each_child(children, [&](const auto & child) {
            auto hint = axis::main(child);
            auto extent = allocate_main_extent(hint, total, available);
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

/// Horizontal flex row of statically known children; see `Stack`.
template<Layout... Children>
constexpr Row<std::decay_t<Children>...> row(Children &&... children)
{
    return {std::tuple<std::decay_t<Children>...>{
        std::forward<Children>(children)...}};
}

/// Horizontal flex row from a runtime-sized range of children, which is
/// stored by value (moved or copied in).
template<LayoutRange Children>
constexpr auto row(Children && children)
{
    return Stack<Axis::row, std::decay_t<Children>>{
        std::forward<Children>(children)};
}

/// Vertical flex column of statically known children; see `Stack`.
template<Layout... Children>
constexpr Column<std::decay_t<Children>...> column(Children &&... children)
{
    return {std::tuple<std::decay_t<Children>...>{
        std::forward<Children>(children)...}};
}

/// Vertical flex column from a runtime-sized range of children, which is
/// stored by value.
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
///
/// Measuring and rendering call `view(item)` again for each item. Width is
/// the widest child's minimum and grows; height is fixed at the sum of the
/// children's minimum heights. Each child is rendered at its minimum
/// height, and rendering stops at the first child that does not fit.
template<typename T, typename ViewFn>
struct Each
{
    /// Borrowed items; must outlive the layout's last render.
    std::span<const T> items;
    /// Maps one item to a layout.
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

/// Vertical layout of `view(item)` for each borrowed item; see `Each`.
template<typename T, typename ViewFn>
auto each(std::span<const T> items, ViewFn && view)
{
    return Each<T, std::decay_t<ViewFn>>{
        items,
        std::forward<ViewFn>(view)};
}

/// `each` over a borrowed vector, which must outlive the layout.
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

/// `each` that takes ownership of the items; see `OwningEach`.
template<typename T, typename ViewFn>
auto each(std::vector<T> && items, ViewFn && view)
{
    return OwningEach<T, std::decay_t<ViewFn>>{
        std::move(items),
        std::forward<ViewFn>(view)};
}

/// Render a span of items by mapping each item to a one-line layout.
///
/// Unlike `each`, every item gets exactly one line regardless of its
/// layout's height hint, and the list always grows horizontally.
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
