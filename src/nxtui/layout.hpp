#pragma once

#include "nxtui/units.hpp"

#include <concepts>
#include <ranges>
#include <tuple>

namespace nxtui {
class RasterView;
}

namespace nxtui::tui {

/// Maps a unit constant (`ch` or `ln`) to its extent type.
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

/// What a layout asks for along one axis: a minimum extent plus a flex
/// factor for sharing leftover space.
///
/// A container gives each child at least `min` along its main axis, then
/// splits whatever is left among children in proportion to `flex`. A flex
/// of zero means the child never grows past `min`. Hints are requests, not
/// guarantees: when space runs short a child may get less than `min`.
template<auto Unit, typename Extent = hint_extent_t<Unit>>
struct SizeHint
{
    /// Minimum extent the layout wants.
    Extent min{};
    /// Share of leftover space; zero for none.
    ratio_t flex{0.0 * one};

    /// Exactly `n`, never growing.
    static constexpr SizeHint fixed(Extent n)
    {
        return {n, 0.0 * one};
    }

    /// No minimum, growing with weight `factor`.
    static constexpr SizeHint grow(ratio_t factor = 1.0 * one)
    {
        return {Extent{}, factor};
    }
};

/// Horizontal size hint, in cells.
using WidthHint = SizeHint<ch>;
/// Vertical size hint, in lines.
using HeightHint = SizeHint<ln>;

/// A value that can be measured and drawn into a raster.
///
/// Measurement and rendering are separate: `width_hint()` and
/// `height_hint()` report what the layout wants, the parent decides the
/// actual `Size`, and `render(raster, size)` draws into a view of
/// that region (the view is already clipped and offset by the parent).
/// Layouts are plain values built fresh for each frame; rendering must not
/// change them, and a layout may be measured and rendered any number of
/// times. Anything a layout borrows (spans, terminals) must stay alive
/// until it is last rendered.
///
/// Implement the three members to add a custom layout, or wrap a callback
/// with `tui::leaf`.
template<typename L>
concept Layout =
    requires(const L & layout, RasterView & raster, Size size) {
        { layout.width_hint() } -> std::convertible_to<WidthHint>;
        { layout.height_hint() } -> std::convertible_to<HeightHint>;
        { layout.render(raster, size) } -> std::same_as<void>;
    };

/// Visit concrete tuple children or a runtime-sized range. Shared by the
/// terminal and graphical compositions; neither requires type erasure.
template<typename... Children, typename F>
constexpr void
for_each_child(const std::tuple<Children...> & children, F && f)
{
    std::apply([&](const auto &... child) { (f(child), ...); }, children);
}

template<typename Children, typename F>
    requires std::ranges::forward_range<const Children>
constexpr void for_each_child(const Children & children, F && f)
{
    for (const auto & child : children)
        f(child);
}

/// Preserve minima and share only positive leftover space. The extent's
/// arithmetic decides quantization: terminal extents truncate to cells,
/// while graphical extents retain fractions until the renderer boundary.
template<typename Hint, typename Extent>
constexpr Extent
allocate_main_extent(Hint hint, Hint total, Extent available)
{
    auto leftover =
        available > total.min ? available - total.min : Extent{};
    auto extent = hint.min;
    if (hint.flex > 0 && total.flex > 0 && leftover.count() > 0)
        extent += leftover * (hint.flex.value() / total.flex.value());
    return extent;
}

} // namespace nxtui::tui
