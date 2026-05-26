#pragma once

/// Terminal grid geometry.
///
/// This is intentionally much smaller than a general units library. It models
/// the dimensions nxt actually needs: columns, rows, ratios, percentages, and
/// terminal positions.

#include <compare>
#include <cstddef>
#include <type_traits>

namespace nxtui {

/// Literal unit for terminal-cell widths.
struct ch_unit
{};
/// Literal unit for terminal-line heights.
struct ln_unit
{};
/// Literal unit for dimensionless ratios.
struct one_unit
{};
/// Literal unit for percentages.
struct percent_unit
{};

/// Character-cell width unit.
inline constexpr ch_unit ch{};

/// Terminal line-height unit.
inline constexpr ln_unit ln{};

/// Dimensionless ratio unit.
inline constexpr one_unit one{};

/// Percent unit. A value of `100 * percent` is equivalent to `1 * one`.
inline constexpr percent_unit percent{};

/// Strong type for horizontal extents measured in terminal cells.
struct width_t
{
    /// Number of cells.
    std::size_t v{};

    /// Return the raw cell count.
    [[nodiscard]] constexpr std::size_t count() const noexcept
    {
        return v;
    }

    constexpr width_t & operator+=(width_t other) noexcept
    {
        v += other.v;
        return *this;
    }

    constexpr width_t & operator-=(width_t other) noexcept
    {
        v -= other.v;
        return *this;
    }

    friend constexpr bool operator==(width_t, width_t) noexcept = default;
    friend constexpr auto operator<=>(width_t, width_t) noexcept = default;
};

/// Strong type for vertical extents measured in terminal lines.
struct height_t
{
    /// Number of lines.
    std::size_t v{};

    /// Return the raw line count.
    [[nodiscard]] constexpr std::size_t count() const noexcept
    {
        return v;
    }

    constexpr height_t & operator+=(height_t other) noexcept
    {
        v += other.v;
        return *this;
    }

    constexpr height_t & operator-=(height_t other) noexcept
    {
        v -= other.v;
        return *this;
    }

    friend constexpr bool operator==(height_t, height_t) noexcept = default;
    friend constexpr auto operator<=>(height_t, height_t) noexcept = default;
};

/// Dimensionless flex or scale ratio.
struct ratio_t
{
    /// Raw ratio value.
    double v{};

    /// Return the raw ratio.
    [[nodiscard]] constexpr double value() const noexcept
    {
        return v;
    }

    constexpr ratio_t & operator+=(ratio_t other) noexcept
    {
        v += other.v;
        return *this;
    }

    friend constexpr bool operator==(ratio_t, ratio_t) noexcept = default;
    friend constexpr auto operator<=>(ratio_t, ratio_t) noexcept = default;
};

/// Percentage value, where 100 means one whole.
struct percent_t
{
    /// Raw percentage value.
    double v{};

    /// Return the raw percentage.
    [[nodiscard]] constexpr double value() const noexcept
    {
        return v;
    }

    /// Convert to a dimensionless ratio.
    [[nodiscard]] constexpr ratio_t ratio() const noexcept
    {
        return {v / 100.0};
    }

    friend constexpr bool operator==(percent_t, percent_t) noexcept = default;
    friend constexpr auto operator<=>(percent_t, percent_t) noexcept = default;
};

/// Arithmetic type accepted by the unit literal operators.
template<typename T>
concept numeric = std::is_arithmetic_v<T>;

/// Compare a ratio against a numeric value.
template<numeric T>
[[nodiscard]] constexpr bool operator>(ratio_t a, T b) noexcept
{
    return a.v > static_cast<double>(b);
}

/// Compare a ratio against a numeric value.
template<numeric T>
[[nodiscard]] constexpr bool operator>=(ratio_t a, T b) noexcept
{
    return a.v >= static_cast<double>(b);
}

/// Compare a ratio against a numeric value.
template<numeric T>
[[nodiscard]] constexpr bool operator<(ratio_t a, T b) noexcept
{
    return a.v < static_cast<double>(b);
}

/// Compare a ratio against a numeric value.
template<numeric T>
[[nodiscard]] constexpr bool operator<=(ratio_t a, T b) noexcept
{
    return a.v <= static_cast<double>(b);
}

/// Create a width from a number of terminal cells.
template<numeric T>
[[nodiscard]] constexpr width_t operator*(T n, ch_unit) noexcept
{
    return {static_cast<std::size_t>(n)};
}

/// Create a height from a number of terminal lines.
template<numeric T>
[[nodiscard]] constexpr height_t operator*(T n, ln_unit) noexcept
{
    return {static_cast<std::size_t>(n)};
}

/// Create a dimensionless ratio.
template<numeric T>
[[nodiscard]] constexpr ratio_t operator*(T n, one_unit) noexcept
{
    return {static_cast<double>(n)};
}

/// Create a percentage value.
template<numeric T>
[[nodiscard]] constexpr percent_t operator*(T n, percent_unit) noexcept
{
    return {static_cast<double>(n)};
}

/// Add widths.
[[nodiscard]] constexpr width_t operator+(width_t a, width_t b) noexcept
{
    return {a.v + b.v};
}

/// Subtract widths.
[[nodiscard]] constexpr width_t operator-(width_t a, width_t b) noexcept
{
    return {a.v - b.v};
}

/// Scale a width by a floating-point factor.
[[nodiscard]] constexpr width_t operator*(width_t a, double b) noexcept
{
    return {static_cast<std::size_t>(static_cast<double>(a.v) * b)};
}

/// Scale a width by a floating-point factor.
[[nodiscard]] constexpr width_t operator*(double a, width_t b) noexcept
{
    return b * a;
}

/// Add heights.
[[nodiscard]] constexpr height_t operator+(height_t a, height_t b) noexcept
{
    return {a.v + b.v};
}

/// Subtract heights.
[[nodiscard]] constexpr height_t operator-(height_t a, height_t b) noexcept
{
    return {a.v - b.v};
}

/// Scale a height by a floating-point factor.
[[nodiscard]] constexpr height_t operator*(height_t a, double b) noexcept
{
    return {static_cast<std::size_t>(static_cast<double>(a.v) * b)};
}

/// Scale a height by a floating-point factor.
[[nodiscard]] constexpr height_t operator*(double a, height_t b) noexcept
{
    return b * a;
}

/// Add ratios.
[[nodiscard]] constexpr ratio_t operator+(ratio_t a, ratio_t b) noexcept
{
    return {a.v + b.v};
}

/// Subtract ratios.
[[nodiscard]] constexpr ratio_t operator-(ratio_t a, ratio_t b) noexcept
{
    return {a.v - b.v};
}

/// Divide a percentage by a scalar.
[[nodiscard]] constexpr percent_t operator/(percent_t a, double b) noexcept
{
    return {a.v / b};
}

/// Scale a percentage by a scalar.
[[nodiscard]] constexpr percent_t operator*(percent_t a, double b) noexcept
{
    return {a.v * b};
}

/// Scale a percentage by a scalar.
[[nodiscard]] constexpr percent_t operator*(double a, percent_t b) noexcept
{
    return b * a;
}

/// Add percentages.
[[nodiscard]] constexpr percent_t operator+(percent_t a, percent_t b) noexcept
{
    return {a.v + b.v};
}

/// Subtract percentages.
[[nodiscard]] constexpr percent_t operator-(percent_t a, percent_t b) noexcept
{
    return {a.v - b.v};
}

/// Compare a percentage with a ratio after converting to a ratio.
[[nodiscard]] constexpr bool operator>=(percent_t a, ratio_t b) noexcept
{
    return a.ratio().v >= b.v;
}

/// Compare a percentage with a ratio after converting to a ratio.
[[nodiscard]] constexpr bool operator>(percent_t a, ratio_t b) noexcept
{
    return a.ratio().v > b.v;
}

/// Compare a percentage with a ratio after converting to a ratio.
[[nodiscard]] constexpr bool operator<=(percent_t a, ratio_t b) noexcept
{
    return a.ratio().v <= b.v;
}

/// Compare a percentage with a ratio after converting to a ratio.
[[nodiscard]] constexpr bool operator<(percent_t a, ratio_t b) noexcept
{
    return a.ratio().v < b.v;
}

/// Zero-based terminal-column origin tag.
struct terminal_origin_t
{};
/// Zero-based terminal-row origin tag.
struct terminal_origin_v_t
{};
/// One-based ANSI-column origin tag.
struct ansi_origin_t
{};
/// One-based ANSI-row origin tag.
struct ansi_origin_v_t
{};

/// Zero-based terminal-column origin.
inline constexpr terminal_origin_t terminal_origin{};
/// Zero-based terminal-row origin.
inline constexpr terminal_origin_v_t terminal_origin_v{};
/// One-based ANSI-column origin.
inline constexpr ansi_origin_t ansi_origin{};
/// One-based ANSI-row origin.
inline constexpr ansi_origin_v_t ansi_origin_v{};

/// Strong type for a zero-based terminal column.
struct col_t
{
    /// Zero-based column index.
    std::size_t v{};

    /// Return the raw zero-based column index.
    [[nodiscard]] constexpr std::size_t index() const noexcept
    {
        return v;
    }

    constexpr col_t & operator+=(width_t dx) noexcept
    {
        v += dx.v;
        return *this;
    }

    friend constexpr bool operator==(col_t, col_t) noexcept = default;
    friend constexpr auto operator<=>(col_t, col_t) noexcept = default;
};

/// Strong type for a zero-based terminal row.
struct row_t
{
    /// Zero-based row index.
    std::size_t v{};

    /// Return the raw zero-based row index.
    [[nodiscard]] constexpr std::size_t index() const noexcept
    {
        return v;
    }

    constexpr row_t & operator+=(height_t dy) noexcept
    {
        v += dy.v;
        return *this;
    }

    friend constexpr bool operator==(row_t, row_t) noexcept = default;
    friend constexpr auto operator<=>(row_t, row_t) noexcept = default;
};

/// ANSI columns share the same representation but are interpreted one-based.
using ansi_col_t = col_t;
/// ANSI rows share the same representation but are interpreted one-based.
using ansi_row_t = row_t;

/// Offset the terminal column origin by a width.
[[nodiscard]] constexpr col_t
operator+(terminal_origin_t, width_t dx) noexcept
{
    return {dx.v};
}

/// Offset the terminal row origin by a height.
[[nodiscard]] constexpr row_t
operator+(terminal_origin_v_t, height_t dy) noexcept
{
    return {dy.v};
}

/// Move a column to the right.
[[nodiscard]] constexpr col_t operator+(col_t p, width_t dx) noexcept
{
    return {p.v + dx.v};
}

/// Move a column to the left.
[[nodiscard]] constexpr col_t operator-(col_t p, width_t dx) noexcept
{
    return {p.v - dx.v};
}

/// Distance between two columns.
[[nodiscard]] constexpr width_t operator-(col_t a, col_t b) noexcept
{
    return {a.v - b.v};
}

/// Convert a column back to a zero-based width from terminal origin.
[[nodiscard]] constexpr width_t
operator-(col_t p, terminal_origin_t) noexcept
{
    return {p.v};
}

/// Convert a zero-based column to a one-based ANSI column value.
[[nodiscard]] constexpr width_t operator-(col_t p, ansi_origin_t) noexcept
{
    return {p.v + 1};
}

/// Move a row down.
[[nodiscard]] constexpr row_t operator+(row_t p, height_t dy) noexcept
{
    return {p.v + dy.v};
}

/// Move a row up.
[[nodiscard]] constexpr row_t operator-(row_t p, height_t dy) noexcept
{
    return {p.v - dy.v};
}

/// Distance between two rows.
[[nodiscard]] constexpr height_t operator-(row_t a, row_t b) noexcept
{
    return {a.v - b.v};
}

/// Convert a row back to a zero-based height from terminal origin.
[[nodiscard]] constexpr height_t
operator-(row_t p, terminal_origin_v_t) noexcept
{
    return {p.v};
}

/// Convert a zero-based row to a one-based ANSI row value.
[[nodiscard]] constexpr height_t
operator-(row_t p, ansi_origin_v_t) noexcept
{
    return {p.v + 1};
}

/// Two-dimensional extent in terminal cells.
struct Size
{
    /// Width in terminal cells.
    width_t w{0 * ch};
    /// Height in terminal lines.
    height_t h{0 * ln};

    /// Construct from width and height.
    constexpr Size(width_t w, height_t h)
        : w{w}
        , h{h}
    {
    }

    /// Construct a zero size.
    constexpr Size() = default;
};

/// Two-dimensional zero-based terminal position.
struct Pos
{
    /// Column coordinate.
    col_t x = terminal_origin + 0 * ch;
    /// Row coordinate.
    row_t y = terminal_origin_v + 0 * ln;

    /// Return the terminal origin.
    [[nodiscard]] static constexpr Pos origin() noexcept
    {
        return {};
    }

    /// Construct a position from offsets from terminal origin.
    [[nodiscard]] static constexpr Pos at(width_t dx, height_t dy) noexcept
    {
        return {terminal_origin + dx, terminal_origin_v + dy};
    }

    /// Move horizontally.
    [[nodiscard]] constexpr Pos operator+(width_t dx) const noexcept
    {
        return {x + dx, y};
    }

    /// Move vertically.
    [[nodiscard]] constexpr Pos operator+(height_t dy) const noexcept
    {
        return {x, y + dy};
    }

    /// Move by a size delta.
    [[nodiscard]] constexpr Pos operator+(Size delta) const noexcept
    {
        return {x + delta.w, y + delta.h};
    }

    /// Difference between two positions as a size delta.
    friend constexpr Size operator-(Pos a, Pos b) noexcept
    {
        return {a.x - b.x, a.y - b.y};
    }

    /// Move by a size delta in place.
    constexpr Pos & operator+=(Size delta) noexcept
    {
        x += delta.w;
        y += delta.h;
        return *this;
    }

    /// Move horizontally in place.
    constexpr Pos & operator+=(width_t dx) noexcept
    {
        x += dx;
        return *this;
    }

    /// Move vertically in place.
    constexpr Pos & operator+=(height_t dy) noexcept
    {
        y += dy;
        return *this;
    }

    /// Return this position as a size from origin.
    [[nodiscard]] constexpr Size from_origin() const noexcept
    {
        return *this - Pos::origin();
    }

    /// Raw zero-based column index.
    [[nodiscard]] constexpr std::size_t col() const noexcept
    {
        return x.index();
    }

    /// Raw zero-based row index.
    [[nodiscard]] constexpr std::size_t row() const noexcept
    {
        return y.index();
    }

    friend constexpr bool operator==(Pos, Pos) noexcept = default;
};

/// Convert a terminal column to the representation used by ANSI writer calls.
[[nodiscard]] constexpr ansi_col_t to_ansi(col_t col) noexcept
{
    return col;
}

/// Convert a terminal row to the representation used by ANSI writer calls.
[[nodiscard]] constexpr ansi_row_t to_ansi(row_t row) noexcept
{
    return row;
}

/// Convert a position's column for ANSI writer calls.
[[nodiscard]] constexpr ansi_col_t to_ansi_x(Pos pos) noexcept
{
    return to_ansi(pos.x);
}

/// Convert a position's row for ANSI writer calls.
[[nodiscard]] constexpr ansi_row_t to_ansi_y(Pos pos) noexcept
{
    return to_ansi(pos.y);
}

} // namespace nxtui
