#pragma once

#include "nxt/units.hpp"

#include <concepts>

namespace nxt {
class RasterView;
}

namespace nxt::tui {

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

template<auto Unit>
struct SizeHint
{
    hint_extent_t<Unit> min{0 * Unit};
    ratio_t flex{0.0 * one};

    static constexpr SizeHint fixed(hint_extent_t<Unit> n)
    {
        return {n, 0.0 * one};
    }

    static constexpr SizeHint grow(ratio_t factor = 1.0 * one)
    {
        return {0 * Unit, factor};
    }
};

using WidthHint = SizeHint<ch>;
using HeightHint = SizeHint<ln>;

template<typename L>
concept Layout =
    requires(const L & layout, RasterView & raster, Size size) {
        { layout.width_hint() } -> std::convertible_to<WidthHint>;
        { layout.height_hint() } -> std::convertible_to<HeightHint>;
        { layout.render(raster, size) } -> std::same_as<void>;
    };

} // namespace nxt::tui
