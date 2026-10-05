#pragma once

#include "nxtui/layout.hpp"
#include "nxtui/style.hpp"
#include "nxtui/ui-units.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// Renderer-neutral graphical compositions. Geometry stays in continuous
/// character/line rhythm units; no Raster, glyph-cell table, or pixels.
namespace nxtui::ui {

using Style = tui::Style;
using tui::bg;
using tui::bold;
using tui::em;
using tui::faint;
using tui::fg;
using tui::italic;
using tui::reverse;
using tui::strikethrough;
using tui::underline;
using WidthHint = tui::SizeHint<nxtui::ch, Width>;
using HeightHint = tui::SizeHint<nxtui::ln, Height>;

struct Measure
{
    WidthHint width;
    HeightHint height;
};

/// Backend-shaped text, including wrapping. The same shaping API supplies
/// measurement and GlyphRun painting; advances are never codepoint counts.
struct ShapedText
{
    Size size;
    virtual ~ShapedText() = default;
};

struct TextMetrics
{
    virtual ~TextMetrics() = default;
    virtual std::shared_ptr<const ShapedText>
    shape(std::string_view text, Width wrap_width, Emphasis emphasis) = 0;
};

struct FillRect
{
    Rect rect;
    Rgba8 color;
    Rect clip;
};

/// Stroke width uses the character rhythm, uniformly in both directions
/// at the renderer boundary. It does not consume layout space.
struct StrokeRect
{
    Rect rect;
    Rgba8 color;
    Width thickness;
    Rect clip;
};

struct GlyphRun
{
    Pos origin;
    std::shared_ptr<const ShapedText> text;
    Rgba8 color;
    Rect clip;
};

using PaintOp = std::variant<FillRect, StrokeRect, GlyphRun>;

struct Frame
{
    Size size;
    std::vector<PaintOp> operations;
};

/// A positioned, clipped paint scope. Child geometry retains its requested
/// size even when its visible clip is smaller; strokes do not gain fake
/// edges at clip boundaries. Custom layouts use this same contract.
struct Context
{
    TextMetrics & metrics;
    Frame & frame;
    Rect bounds;
    Rect clip;
    Style style;

    Context child(Rect local) const
    {
        local.origin = bounds.origin + local.origin;
        return {metrics, frame, local, intersect(clip, local), style};
    }

    Context styled(Style overlay) const
    {
        auto result = *this;
        result.style = style | overlay;
        return result;
    }

    void fill(Rgba8 color) const
    {
        if (!clip.empty() && !color.is_transparent())
            frame.operations.emplace_back(FillRect{bounds, color, clip});
    }

    void stroke(Rgba8 color, Width thickness) const
    {
        if (!clip.empty() && thickness > Width{})
            frame.operations.emplace_back(
                StrokeRect{bounds, color, thickness, clip});
    }
};

template<typename L>
concept Layout =
    requires(const L & layout, const Context & context, Width width) {
        { layout.measure(context, width) } -> std::same_as<Measure>;
        { layout.paint(context) } -> std::same_as<void>;
    };

struct Text
{
    std::string value;
    Style style;
    bool wrap = true;

    std::shared_ptr<const ShapedText>
    shape(const Context & context, Width width) const
    {
        return context.metrics.shape(
            value,
            wrap ? width : Width{std::numeric_limits<double>::infinity()},
            (context.style | style).em);
    }

    Measure measure(const Context & context, Width width) const
    {
        if (width <= Width{})
            return {};
        auto run = shape(context, width);
        return {
            WidthHint::fixed(run->size.w), HeightHint::fixed(run->size.h)};
    }

    void paint(const Context & context) const
    {
        if (context.clip.empty())
            return;
        auto run = shape(context, context.bounds.size.w);
        auto resolved = context.style | style;
        auto reversed = has_emphasis(resolved.em, Emphasis::reverse);
        if (reversed)
            std::swap(resolved.fg, resolved.bg);
        if (style.bg != DEFAULT_COLOR || reversed) {
            auto rect = Rect{context.bounds.origin, run->size};
            auto background = intersect(rect, context.clip);
            if (!background.empty() && !resolved.bg.is_transparent())
                context.frame.operations.emplace_back(
                    FillRect{rect, resolved.bg, background});
        }
        context.frame.operations.emplace_back(
            GlyphRun{
                context.bounds.origin,
                std::move(run),
                resolved.fg,
                context.clip});
    }
};

inline Text text(std::string value, Style style = {}, bool wrap = true)
{
    return {std::move(value), style, wrap};
}

struct Space
{
    Measure hints;

    Measure measure(const Context &, Width) const
    {
        return hints;
    }

    void paint(const Context &) const {}
};

inline Space space(Width width = {}, Height height = {})
{
    return {{WidthHint::fixed(width), HeightHint::fixed(height)}};
}

inline Space stretch()
{
    return {{WidthHint::grow(), HeightHint::grow()}};
}

/// One-child box: style/padding, sizing hints, and an optional outline.
/// Helpers below compose boxes exactly like terminal value decorators.
template<Layout Child>
struct Box
{
    Child child;
    Insets insets{};
    Style style{};
    std::optional<WidthHint> width{};
    std::optional<HeightHint> height{};
    std::optional<Rgba8> outline{};
    Width thickness{0.125};

    Measure measure(const Context & context, Width available) const
    {
        auto horizontal = insets.left + insets.right;
        if (width && width->flex.value() == 0)
            available = width->min;
        auto result = child.measure(
            context.styled(style),
            std::max(Width{}, available - horizontal));
        result.width.min += horizontal;
        result.height.min += insets.top + insets.bottom;
        if (width)
            result.width = *width;
        if (height)
            result.height = *height;
        return result;
    }

    void paint(const Context & context) const
    {
        auto styled = context.styled(style);
        if (style.bg != DEFAULT_COLOR)
            styled.fill(styled.style.bg);
        auto inside = styled.child(
            {{insets.left, insets.top},
             {std::max(
                  Width{},
                  context.bounds.size.w - insets.left - insets.right),
              std::max(
                  Height{},
                  context.bounds.size.h - insets.top - insets.bottom)}});
        child.paint(inside);
        if (outline)
            styled.stroke(*outline, thickness);
    }
};

template<Layout L>
auto padding(Insets insets, L child)
{
    return Box<L>{.child = std::move(child), .insets = insets};
}

template<Layout L>
auto surface(Style style, L child)
{
    return Box<L>{.child = std::move(child), .style = style};
}

template<Layout L>
auto border(Rgba8 color, L child, Width thickness = 0.125 * ch)
{
    return Box<L>{
        .child = std::move(child),
        .outline = color,
        .thickness = thickness};
}

template<Layout L>
auto fixed_width(Width width, L child)
{
    return Box<L>{
        .child = std::move(child), .width = WidthHint::fixed(width)};
}

template<Layout L>
auto fixed_height(Height height, L child)
{
    return Box<L>{
        .child = std::move(child), .height = HeightHint::fixed(height)};
}

template<Layout L>
auto grow_width(L child, double weight = 1)
{
    return Box<L>{
        .child = std::move(child), .width = WidthHint::grow(weight * one)};
}

template<Layout L>
auto grow_height(L child, double weight = 1)
{
    return Box<L>{
        .child = std::move(child),
        .height = HeightHint::grow(weight * one)};
}

template<Axis A, typename Children>
struct Stack
{
    Children children;
    Extent<A> gap{};

    auto main_hint(const Context & context, Width available) const
    {
        using Hint =
            std::conditional_t<A == Axis::row, WidthHint, HeightHint>;
        auto total = Hint{};
        std::size_t count = 0;
        tui::for_each_child(children, [&](const auto & child) {
            auto measured = child.measure(context, available);
            auto hint = [&] {
                if constexpr (A == Axis::row)
                    return measured.width;
                else
                    return measured.height;
            }();
            total.min += hint.min;
            total.flex += hint.flex;
            ++count;
        });
        if (count > 1)
            total.min += gap * static_cast<double>(count - 1);
        return total;
    }

    Measure measure(const Context & context, Width available) const
    {
        auto total = main_hint(context, available);
        auto cross = std::conditional_t<A == Axis::row, Height, Width>{};
        tui::for_each_child(children, [&](const auto & child) {
            auto width = available;
            if constexpr (A == Axis::row)
                width = tui::allocate_main_extent(
                    child.measure(context, available).width,
                    total,
                    available);
            auto measured = child.measure(context, width);
            if constexpr (A == Axis::row)
                cross = std::max(cross, measured.height.min);
            else
                cross = std::max(cross, measured.width.min);
        });
        if constexpr (A == Axis::row)
            return {total, HeightHint::fixed(cross)};
        else
            return {{cross, 1.0 * one}, total};
    }

    void paint(const Context & context) const
    {
        auto total = main_hint(context, context.bounds.size.w);
        auto available = [&] {
            if constexpr (A == Axis::row)
                return context.bounds.size.w;
            else
                return context.bounds.size.h;
        }();
        auto cursor = Extent<A>{};
        tui::for_each_child(children, [&](const auto & child) {
            auto measured = child.measure(context, context.bounds.size.w);
            auto hint = [&] {
                if constexpr (A == Axis::row)
                    return measured.width;
                else
                    return measured.height;
            }();
            auto extent = tui::allocate_main_extent(hint, total, available);
            Rect local;
            if constexpr (A == Axis::row)
                local = {{cursor, {}}, {extent, context.bounds.size.h}};
            else
                local = {{{}, cursor}, {context.bounds.size.w, extent}};
            if (extent > Extent<A>{})
                child.paint(context.child(local));
            cursor += extent + gap;
        });
    }
};

template<Layout... L>
auto row(Width gap, L... children)
{
    return Stack<Axis::row, std::tuple<L...>>{
        std::tuple{std::move(children)...}, gap};
}

template<Layout... L>
auto column(Height gap, L... children)
{
    return Stack<Axis::column, std::tuple<L...>>{
        std::tuple{std::move(children)...}, gap};
}

template<Layout... L>
auto row(L... children)
{
    return row(Width{}, std::move(children)...);
}

template<Layout... L>
auto column(L... children)
{
    return column(Height{}, std::move(children)...);
}

template<typename R>
    requires std::ranges::forward_range<const R>
auto column(Height gap, R children)
{
    return Stack<Axis::column, R>{std::move(children), gap};
}

template<typename R>
    requires std::ranges::forward_range<const R>
auto row(Width gap, R children)
{
    return Stack<Axis::row, R>{std::move(children), gap};
}

/// Build an owning display list. Layout values can be destroyed afterward;
/// the frame owns its shaped runs. The text backend must outlive its runs.
inline Frame paint(
    const Layout auto & layout,
    TextMetrics & metrics,
    Size size,
    Style style = {Rgba8(230, 233, 240), Rgba8(16, 21, 31), Emphasis::none})
{
    auto frame = Frame{size, {}};
    auto bounds = Rect{{}, size};
    auto context = Context{metrics, frame, bounds, bounds, style};
    layout.paint(context);
    return frame;
}

} // namespace nxtui::ui
