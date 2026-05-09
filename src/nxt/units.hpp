#pragma once

/// Terminal grid geometry.
///
/// This is intentionally much smaller than a general units library. It models
/// the dimensions nxt actually needs: columns, rows, ratios, percentages, and
/// terminal positions.

#include <compare>
#include <cstddef>
#include <type_traits>

namespace nxt {

struct ch_unit
{};
struct ln_unit
{};
struct one_unit
{};
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

struct width_t
{
    std::size_t v{};

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

struct height_t
{
    std::size_t v{};

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

struct ratio_t
{
    double v{};

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

struct percent_t
{
    double v{};

    [[nodiscard]] constexpr double value() const noexcept
    {
        return v;
    }

    [[nodiscard]] constexpr ratio_t ratio() const noexcept
    {
        return {v / 100.0};
    }

    friend constexpr bool operator==(percent_t, percent_t) noexcept = default;
    friend constexpr auto operator<=>(percent_t, percent_t) noexcept = default;
};

template<typename T>
concept numeric = std::is_arithmetic_v<T>;

template<numeric T>
[[nodiscard]] constexpr bool operator>(ratio_t a, T b) noexcept
{
    return a.v > static_cast<double>(b);
}

template<numeric T>
[[nodiscard]] constexpr bool operator>=(ratio_t a, T b) noexcept
{
    return a.v >= static_cast<double>(b);
}

template<numeric T>
[[nodiscard]] constexpr bool operator<(ratio_t a, T b) noexcept
{
    return a.v < static_cast<double>(b);
}

template<numeric T>
[[nodiscard]] constexpr bool operator<=(ratio_t a, T b) noexcept
{
    return a.v <= static_cast<double>(b);
}

template<numeric T>
[[nodiscard]] constexpr width_t operator*(T n, ch_unit) noexcept
{
    return {static_cast<std::size_t>(n)};
}

template<numeric T>
[[nodiscard]] constexpr height_t operator*(T n, ln_unit) noexcept
{
    return {static_cast<std::size_t>(n)};
}

template<numeric T>
[[nodiscard]] constexpr ratio_t operator*(T n, one_unit) noexcept
{
    return {static_cast<double>(n)};
}

template<numeric T>
[[nodiscard]] constexpr percent_t operator*(T n, percent_unit) noexcept
{
    return {static_cast<double>(n)};
}

[[nodiscard]] constexpr width_t operator+(width_t a, width_t b) noexcept
{
    return {a.v + b.v};
}

[[nodiscard]] constexpr width_t operator-(width_t a, width_t b) noexcept
{
    return {a.v - b.v};
}

[[nodiscard]] constexpr width_t operator*(width_t a, double b) noexcept
{
    return {static_cast<std::size_t>(static_cast<double>(a.v) * b)};
}

[[nodiscard]] constexpr width_t operator*(double a, width_t b) noexcept
{
    return b * a;
}

[[nodiscard]] constexpr height_t operator+(height_t a, height_t b) noexcept
{
    return {a.v + b.v};
}

[[nodiscard]] constexpr height_t operator-(height_t a, height_t b) noexcept
{
    return {a.v - b.v};
}

[[nodiscard]] constexpr height_t operator*(height_t a, double b) noexcept
{
    return {static_cast<std::size_t>(static_cast<double>(a.v) * b)};
}

[[nodiscard]] constexpr height_t operator*(double a, height_t b) noexcept
{
    return b * a;
}

[[nodiscard]] constexpr ratio_t operator+(ratio_t a, ratio_t b) noexcept
{
    return {a.v + b.v};
}

[[nodiscard]] constexpr ratio_t operator-(ratio_t a, ratio_t b) noexcept
{
    return {a.v - b.v};
}

[[nodiscard]] constexpr percent_t operator/(percent_t a, double b) noexcept
{
    return {a.v / b};
}

[[nodiscard]] constexpr percent_t operator*(percent_t a, double b) noexcept
{
    return {a.v * b};
}

[[nodiscard]] constexpr percent_t operator*(double a, percent_t b) noexcept
{
    return b * a;
}

[[nodiscard]] constexpr percent_t operator+(percent_t a, percent_t b) noexcept
{
    return {a.v + b.v};
}

[[nodiscard]] constexpr percent_t operator-(percent_t a, percent_t b) noexcept
{
    return {a.v - b.v};
}

[[nodiscard]] constexpr bool operator>=(percent_t a, ratio_t b) noexcept
{
    return a.ratio().v >= b.v;
}

[[nodiscard]] constexpr bool operator>(percent_t a, ratio_t b) noexcept
{
    return a.ratio().v > b.v;
}

[[nodiscard]] constexpr bool operator<=(percent_t a, ratio_t b) noexcept
{
    return a.ratio().v <= b.v;
}

[[nodiscard]] constexpr bool operator<(percent_t a, ratio_t b) noexcept
{
    return a.ratio().v < b.v;
}

struct terminal_origin_t
{};
struct terminal_origin_v_t
{};
struct ansi_origin_t
{};
struct ansi_origin_v_t
{};

inline constexpr terminal_origin_t terminal_origin{};
inline constexpr terminal_origin_v_t terminal_origin_v{};
inline constexpr ansi_origin_t ansi_origin{};
inline constexpr ansi_origin_v_t ansi_origin_v{};

struct col_t
{
    std::size_t v{};

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

struct row_t
{
    std::size_t v{};

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

using ansi_col_t = col_t;
using ansi_row_t = row_t;

[[nodiscard]] constexpr col_t
operator+(terminal_origin_t, width_t dx) noexcept
{
    return {dx.v};
}

[[nodiscard]] constexpr row_t
operator+(terminal_origin_v_t, height_t dy) noexcept
{
    return {dy.v};
}

[[nodiscard]] constexpr col_t operator+(col_t p, width_t dx) noexcept
{
    return {p.v + dx.v};
}

[[nodiscard]] constexpr col_t operator-(col_t p, width_t dx) noexcept
{
    return {p.v - dx.v};
}

[[nodiscard]] constexpr width_t operator-(col_t a, col_t b) noexcept
{
    return {a.v - b.v};
}

[[nodiscard]] constexpr width_t
operator-(col_t p, terminal_origin_t) noexcept
{
    return {p.v};
}

[[nodiscard]] constexpr width_t operator-(col_t p, ansi_origin_t) noexcept
{
    return {p.v + 1};
}

[[nodiscard]] constexpr row_t operator+(row_t p, height_t dy) noexcept
{
    return {p.v + dy.v};
}

[[nodiscard]] constexpr row_t operator-(row_t p, height_t dy) noexcept
{
    return {p.v - dy.v};
}

[[nodiscard]] constexpr height_t operator-(row_t a, row_t b) noexcept
{
    return {a.v - b.v};
}

[[nodiscard]] constexpr height_t
operator-(row_t p, terminal_origin_v_t) noexcept
{
    return {p.v};
}

[[nodiscard]] constexpr height_t
operator-(row_t p, ansi_origin_v_t) noexcept
{
    return {p.v + 1};
}

struct Size
{
    width_t w{0 * ch};
    height_t h{0 * ln};

    constexpr Size(width_t w, height_t h)
        : w{w}
        , h{h}
    {
    }

    constexpr Size() = default;
};

struct Pos
{
    col_t x = terminal_origin + 0 * ch;
    row_t y = terminal_origin_v + 0 * ln;

    [[nodiscard]] static constexpr Pos origin() noexcept
    {
        return {};
    }

    [[nodiscard]] static constexpr Pos at(width_t dx, height_t dy) noexcept
    {
        return {terminal_origin + dx, terminal_origin_v + dy};
    }

    [[nodiscard]] constexpr Pos operator+(width_t dx) const noexcept
    {
        return {x + dx, y};
    }

    [[nodiscard]] constexpr Pos operator+(height_t dy) const noexcept
    {
        return {x, y + dy};
    }

    [[nodiscard]] constexpr Pos operator+(Size delta) const noexcept
    {
        return {x + delta.w, y + delta.h};
    }

    friend constexpr Size operator-(Pos a, Pos b) noexcept
    {
        return {a.x - b.x, a.y - b.y};
    }

    constexpr Pos & operator+=(Size delta) noexcept
    {
        x += delta.w;
        y += delta.h;
        return *this;
    }

    constexpr Pos & operator+=(width_t dx) noexcept
    {
        x += dx;
        return *this;
    }

    constexpr Pos & operator+=(height_t dy) noexcept
    {
        y += dy;
        return *this;
    }

    [[nodiscard]] constexpr Size from_origin() const noexcept
    {
        return *this - Pos::origin();
    }

    [[nodiscard]] constexpr std::size_t col() const noexcept
    {
        return x.index();
    }

    [[nodiscard]] constexpr std::size_t row() const noexcept
    {
        return y.index();
    }

    friend constexpr bool operator==(Pos, Pos) noexcept = default;
};

[[nodiscard]] constexpr ansi_col_t to_ansi(col_t col) noexcept
{
    return col;
}

[[nodiscard]] constexpr ansi_row_t to_ansi(row_t row) noexcept
{
    return row;
}

[[nodiscard]] constexpr ansi_col_t to_ansi_x(Pos pos) noexcept
{
    return to_ansi(pos.x);
}

[[nodiscard]] constexpr ansi_row_t to_ansi_y(Pos pos) noexcept
{
    return to_ansi(pos.y);
}

} // namespace nxt
