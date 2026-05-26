#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nxtui::chart {

struct value_range
{
    double lo = 0.0;
    double hi = 1.0;
};

struct cell_slice
{
    double begin = 0.0;
    double end = 0.0;

    constexpr double width() const
    {
        return end - begin;
    }

    constexpr double center() const
    {
        return begin + width() / 2.0;
    }
};

inline cell_slice unit_cell(std::size_t index, std::size_t cells)
{
    if (cells == 0)
        return {};

    auto scale = 1.0 / static_cast<double>(cells);
    return {
        static_cast<double>(index) * scale,
        static_cast<double>(index + 1) * scale,
    };
}

inline std::size_t eighth_index(double fraction)
{
    return static_cast<std::size_t>(
        std::round(std::clamp(fraction, 0.0, 1.0) * 8.0));
}

inline double coverage_before(double edge, cell_slice cell)
{
    auto w = cell.width();
    if (w <= 0.0)
        return 0.0;
    return std::clamp((edge - cell.begin) / w, 0.0, 1.0);
}

template<typename Project>
inline std::string project_cells(std::size_t cells, Project && project)
{
    auto out = std::string{};
    for (std::size_t i = 0; i != cells; ++i)
        out += project(unit_cell(i, cells), i);
    return out;
}

inline std::string_view vertical_eighth(double fraction)
{
    static constexpr auto blocks = std::array<std::string_view, 9>{
        " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█",
    };
    return blocks[eighth_index(fraction)];
}

inline std::string_view horizontal_eighth(double fraction)
{
    static constexpr auto blocks = std::array<std::string_view, 9>{
        " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█",
    };
    return blocks[eighth_index(fraction)];
}

inline std::string_view horizontal_eighth_from_right(double fraction)
{
    // Unicode has a complete left-flushed eighth-block family, but only
    // right one-eighth and right half. Use the closest right-flushed glyphs
    // for range starts, falling back to full block for mostly-filled cells.
    static constexpr auto blocks = std::array<std::string_view, 9>{
        " ", "▕", "▕", "▐", "▐", "▐", "█", "█", "█",
    };
    return blocks[eighth_index(fraction)];
}

inline std::string_view
vertical_cell(double fraction, std::size_t row, std::size_t rows)
{
    if (rows == 0)
        return " ";

    auto filled = std::clamp(fraction, 0.0, 1.0)
                  * static_cast<double>(rows);
    auto local = filled - static_cast<double>(rows - row - 1);
    return vertical_eighth(local);
}

inline std::string progress_bar(double fraction, std::size_t cells)
{
    return project_cells(
        cells,
        [=](cell_slice cell, std::size_t) -> std::string_view {
            return horizontal_eighth(coverage_before(fraction, cell));
        });
}

inline std::string
range_bar(double begin, double end, std::size_t cells)
{
    begin = std::clamp(begin, 0.0, 1.0);
    end = std::clamp(end, begin, 1.0);
    return project_cells(
        cells,
        [=](cell_slice cell, std::size_t) -> std::string_view {
            auto before_begin = coverage_before(begin, cell);
            auto before_end = coverage_before(end, cell);
            auto coverage = before_end - before_begin;
            if (coverage <= 0.0)
                return " ";
            if (before_begin > 0.0 && before_end >= 1.0)
                return horizontal_eighth_from_right(coverage);
            return horizontal_eighth(coverage);
        });
}

inline value_range dynamic_range(std::span<const double> values)
{
    auto [lo_it, hi_it] = std::minmax_element(values.begin(), values.end());
    auto range = value_range{*lo_it, *hi_it};
    if (std::abs(range.hi - range.lo) < 1e-9)
        range.hi = range.lo + 1.0;
    return range;
}

/// Render the last `cells` values as a one-cell-per-sample block sparkline.
inline std::string sparkline(
    std::span<const double> values,
    std::size_t cells,
    value_range range)
{
    if (cells == 0)
        return {};
    if (values.empty())
        return std::string(cells, ' ');

    if (std::abs(range.hi - range.lo) < 1e-9)
        range.hi = range.lo + 1.0;

    auto take = std::min(values.size(), cells);
    auto pad = cells - take;
    auto offset = values.size() - take;

    return project_cells(
        cells,
        [&](cell_slice, std::size_t i) -> std::string_view {
            if (i < pad)
                return " ";

            auto value = values[offset + i - pad];
            return vertical_eighth(
                (value - range.lo) / (range.hi - range.lo));
        });
}

/// Render a sparkline normalized to the observed values.
inline std::string sparkline(std::span<const double> values, std::size_t cells)
{
    if (values.empty())
        return sparkline(values, cells, value_range{});
    return sparkline(values, cells, dynamic_range(values));
}

inline std::vector<std::string> sparkline_rows(
    std::span<const double> values,
    std::size_t cells,
    std::size_t rows,
    value_range range)
{
    auto out = std::vector<std::string>(rows);
    for (auto & row : out)
        row.reserve(cells);
    if (rows == 0)
        return out;
    if (cells == 0 || values.empty()) {
        for (auto & row : out)
            row.assign(cells, ' ');
        return out;
    }

    if (std::abs(range.hi - range.lo) < 1e-9)
        range.hi = range.lo + 1.0;

    auto take = std::min(values.size(), cells);
    auto pad = cells - take;
    auto offset = values.size() - take;
    for (std::size_t y = 0; y != rows; ++y) {
        for (std::size_t x = 0; x != cells; ++x) {
            if (x < pad) {
                out[y] += " ";
                continue;
            }

            auto value = values[offset + x - pad];
            out[y] += vertical_cell(
                (value - range.lo) / (range.hi - range.lo),
                y,
                rows);
        }
    }

    return out;
}

inline std::vector<std::string> sparkline_rows(
    std::span<const double> values,
    std::size_t cells,
    std::size_t rows)
{
    if (values.empty())
        return sparkline_rows(values, cells, rows, value_range{});
    return sparkline_rows(values, cells, rows, dynamic_range(values));
}

inline std::array<std::string, 2> sparkline2(
    std::span<const double> values,
    std::size_t cells,
    value_range range)
{
    auto rows = sparkline_rows(values, cells, 2, range);
    return {rows[0], rows[1]};
}

inline std::array<std::string, 2>
sparkline2(std::span<const double> values, std::size_t cells)
{
    if (values.empty())
        return sparkline2(values, cells, value_range{});
    return sparkline2(values, cells, dynamic_range(values));
}

} // namespace nxtui::chart
