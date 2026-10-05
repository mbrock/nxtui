#pragma once

#include <algorithm>
#include <compare>
#include <type_traits>

namespace nxtui::ui {

enum class Axis { row, column };

/// Continuous rhythm extents, not pixels or terminal cell counts. Signed
/// values also express positions (for example a scrolled content origin).
template<Axis A>
class Extent
{
public:
    constexpr Extent() = default;

    explicit constexpr Extent(double value)
        : value_(value)
    {
    }

    constexpr double count() const
    {
        return value_;
    }

    constexpr auto operator<=>(const Extent &) const = default;

    constexpr Extent & operator+=(Extent other)
    {
        value_ += other.value_;
        return *this;
    }

    constexpr Extent & operator-=(Extent other)
    {
        value_ -= other.value_;
        return *this;
    }

    friend constexpr Extent operator+(Extent a, Extent b)
    {
        return a += b;
    }

    friend constexpr Extent operator-(Extent a, Extent b)
    {
        return a -= b;
    }

    friend constexpr Extent operator-(Extent a)
    {
        return Extent{-a.value_};
    }

    friend constexpr Extent operator*(Extent a, double factor)
    {
        return Extent{a.value_ * factor};
    }

    friend constexpr Extent operator/(Extent a, double divisor)
    {
        return Extent{a.value_ / divisor};
    }
private:
    double value_ = 0;
};

using Width = Extent<Axis::row>;
using Height = Extent<Axis::column>;

template<Axis A>
struct Unit
{};

inline constexpr Unit<Axis::row> ch;
inline constexpr Unit<Axis::column> ln;

template<typename Number, Axis A>
    requires std::is_arithmetic_v<Number>
constexpr Extent<A> operator*(Number value, Unit<A>)
{
    return Extent<A>{static_cast<double>(value)};
}

struct Size
{
    Width w;
    Height h;
    constexpr bool operator==(const Size &) const = default;
};

struct Pos
{
    Width x;
    Height y;

    friend constexpr Pos operator+(Pos a, Pos b)
    {
        return {a.x + b.x, a.y + b.y};
    }

    constexpr bool operator==(const Pos &) const = default;
};

struct Rect
{
    Pos origin;
    Size size;

    constexpr bool empty() const
    {
        return size.w <= Width{} || size.h <= Height{};
    }

    constexpr bool contains(Pos p) const
    {
        return !empty() && p.x >= origin.x && p.y >= origin.y
               && p.x < origin.x + size.w && p.y < origin.y + size.h;
    }

    constexpr bool operator==(const Rect &) const = default;
};

constexpr Rect intersect(Rect a, Rect b)
{
    auto x = std::max(a.origin.x, b.origin.x);
    auto y = std::max(a.origin.y, b.origin.y);
    return {
        {x, y},
        {std::max(
             Width{},
             std::min(a.origin.x + a.size.w, b.origin.x + b.size.w) - x),
         std::max(
             Height{},
             std::min(a.origin.y + a.size.h, b.origin.y + b.size.h) - y)}};
}

struct Insets
{
    Width left, right;
    Height top, bottom;

    static constexpr Insets symmetric(Width horizontal, Height vertical)
    {
        return {horizontal, horizontal, vertical, vertical};
    }
};

} // namespace nxtui::ui
