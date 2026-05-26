#pragma once

#include <cstdint>
#include <ostream>

namespace nxtui {

/// Opaque 24-bit RGB color.
struct Rgb8
{
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

/// Terminal color: packed into 32 bits.
struct Rgba8
{
    std::uint32_t value;

    constexpr Rgba8(
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b,
        std::uint8_t a = 255) noexcept
        : value(r | (g << 8) | (b << 16) | (a << 24))
    {
    }

    constexpr Rgba8(Rgb8 rgb, std::uint8_t a = 255) noexcept
        : value(rgb.r | (rgb.g << 8) | (rgb.b << 16) | (a << 24))
    {
    }

    static constexpr Rgba8 from_raw(std::uint32_t v) noexcept
    {
        Rgba8 c{0, 0, 0, 0};
        c.value = v;
        return c;
    }

    static constexpr Rgba8 terminal_default() noexcept
    {
        return from_raw(0x00000100);
    }

    static constexpr Rgba8 palette(std::uint8_t index) noexcept
    {
        return from_raw(index);
    }

    static constexpr Rgba8 black() noexcept { return palette(0); }
    static constexpr Rgba8 red() noexcept { return palette(1); }
    static constexpr Rgba8 green() noexcept { return palette(2); }
    static constexpr Rgba8 yellow() noexcept { return palette(3); }
    static constexpr Rgba8 blue() noexcept { return palette(4); }
    static constexpr Rgba8 magenta() noexcept { return palette(5); }
    static constexpr Rgba8 cyan() noexcept { return palette(6); }
    static constexpr Rgba8 white() noexcept { return palette(7); }
    static constexpr Rgba8 bright_black() noexcept { return palette(8); }
    static constexpr Rgba8 bright_red() noexcept { return palette(9); }
    static constexpr Rgba8 bright_green() noexcept { return palette(10); }
    static constexpr Rgba8 bright_yellow() noexcept { return palette(11); }
    static constexpr Rgba8 bright_blue() noexcept { return palette(12); }
    static constexpr Rgba8 bright_magenta() noexcept { return palette(13); }
    static constexpr Rgba8 bright_cyan() noexcept { return palette(14); }
    static constexpr Rgba8 bright_white() noexcept { return palette(15); }

    static constexpr Rgba8 transparent() noexcept
    {
        return from_raw(0x00000200);
    }

    [[nodiscard]] constexpr bool is_true_color() const noexcept
    {
        return (value >> 24) > 0;
    }

    [[nodiscard]] constexpr bool is_palette() const noexcept
    {
        return value <= 0xFF;
    }

    [[nodiscard]] constexpr bool is_terminal_default() const noexcept
    {
        return value == 0x00000100;
    }

    [[nodiscard]] constexpr bool is_transparent() const noexcept
    {
        return value == 0x00000200;
    }

    [[nodiscard]] constexpr std::uint8_t palette_index() const noexcept
    {
        return static_cast<std::uint8_t>(value & 0xFF);
    }

    [[nodiscard]] constexpr std::uint8_t r() const noexcept
    {
        return value & 0xFF;
    }

    [[nodiscard]] constexpr std::uint8_t g() const noexcept
    {
        return (value >> 8) & 0xFF;
    }

    [[nodiscard]] constexpr std::uint8_t b() const noexcept
    {
        return (value >> 16) & 0xFF;
    }

    [[nodiscard]] constexpr std::uint8_t a() const noexcept
    {
        return (value >> 24) & 0xFF;
    }

    [[nodiscard]] constexpr Rgb8 to_rgb() const noexcept
    {
        return Rgb8{r(), g(), b()};
    }

    constexpr auto operator<=>(const Rgba8 &) const = default;

    friend std::ostream & operator<<(std::ostream & os, const Rgba8 & c);
};

enum class Emphasis : std::uint8_t {
    none = 0,
    bold = 1 << 0,
    faint = 1 << 1,
    italic = 1 << 2,
    underline = 1 << 3,
    blink = 1 << 4,
    reverse = 1 << 5,
    conceal = 1 << 6,
    strikethrough = 1 << 7,
};

constexpr Emphasis operator|(Emphasis a, Emphasis b) noexcept
{
    return static_cast<Emphasis>(
        static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr Emphasis operator&(Emphasis a, Emphasis b) noexcept
{
    return static_cast<Emphasis>(
        static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr Emphasis & operator|=(Emphasis & a, Emphasis b) noexcept
{
    return a = a | b;
}

constexpr bool has_emphasis(Emphasis set, Emphasis flag) noexcept
{
    return (set & flag) != Emphasis::none;
}

inline constexpr Emphasis DEFAULT_EMPHASIS = Emphasis::none;
inline constexpr Rgba8 DEFAULT_COLOR = Rgba8::terminal_default();

} // namespace nxtui
