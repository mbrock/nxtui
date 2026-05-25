#pragma once

#include "nxt/sparkline.hpp"
#include "nxt/tui.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace nxt::tui {

inline void render_sparkline(
    RasterView & r,
    Size size,
    std::span<const double> values,
    std::optional<nxt::chart::value_range> range,
    Style style)
{
    std::ranges::fill(r.glyphs(), 32);
    if (style.fg != DEFAULT_COLOR)
        std::ranges::fill(r.fgs(), style.fg);
    if (style.bg != DEFAULT_COLOR)
        std::ranges::fill(r.bgs(), style.bg);
    if (style.em != DEFAULT_EMPHASIS)
        std::ranges::fill(r.ems(), style.em);

    auto cols = size.w.count();
    auto rows = size.h.count();
    if (cols == 0 || rows == 0 || values.empty())
        return;

    auto scale = range ? *range : nxt::chart::dynamic_range(values);
    if (std::abs(scale.hi - scale.lo) < 1e-9)
        scale.hi = scale.lo + 1.0;

    auto take = std::min(values.size(), cols);
    auto pad = cols - take;
    auto offset = values.size() - take;

    for (std::size_t x = 0; x != take; ++x) {
        auto value = values[offset + x];
        auto fraction = (value - scale.lo) / (scale.hi - scale.lo);
        for (std::size_t y = 0; y != rows; ++y) {
            auto glyph = nxt::chart::vertical_cell(fraction, y, rows);
            if (glyph == " ")
                continue;
            r.write_text(Pos::at((pad + x) * ch, y * ln), glyph);
        }
    }
}

/// Create a sparkline layout of any fixed terminal height.
inline auto sparkline(
    std::vector<double> values,
    height_t height,
    Style style = {},
    std::optional<nxt::chart::value_range> range = std::nullopt)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(height),
        [values = std::move(values), range, style](RasterView & r, Size size) {
            render_sparkline(r, size, values, range, style);
        });
}

/// Create a sparkline layout of any fixed terminal height from borrowed values.
inline auto sparkline(
    std::span<const double> values,
    height_t height,
    Style style = {},
    std::optional<nxt::chart::value_range> range = std::nullopt)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(height),
        [values, range, style](RasterView & r, Size size) {
            render_sparkline(r, size, values, range, style);
        });
}

/// Create a one-line sparkline layout from a copied series.
inline auto sparkline(
    std::vector<double> values,
    Style style = {},
    std::optional<nxt::chart::value_range> range = std::nullopt)
{
    return sparkline(std::move(values), 1 * ln, style, range);
}

/// Create a one-line sparkline layout from borrowed values.
inline auto sparkline(
    std::span<const double> values,
    Style style = {},
    std::optional<nxt::chart::value_range> range = std::nullopt)
{
    return sparkline(values, 1 * ln, style, range);
}

/// Create a two-line sparkline layout from a copied series.
inline auto sparkline2(
    std::vector<double> values,
    Style style = {},
    std::optional<nxt::chart::value_range> range = std::nullopt)
{
    return sparkline(std::move(values), 2 * ln, style, range);
}

/// Create a two-line sparkline layout from borrowed values.
inline auto sparkline2(
    std::span<const double> values,
    Style style = {},
    std::optional<nxt::chart::value_range> range = std::nullopt)
{
    return sparkline(values, 2 * ln, style, range);
}

} // namespace nxt::tui
