#pragma once

#include <cstdint>
#include <ostream>

namespace nxtui {

/// Plain 24-bit RGB triple, used for true-color values and theme palettes.
struct Rgb8
{
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

/// Cell color: a true-color RGB value, a 256-color palette index, or one
/// of two sentinels, packed into 32 bits.
///
/// The encoding of `value` decides the kind:
/// - `0x00..0xFF`: palette index (`palette(i)`, `red()`, `bright_cyan()`,
///   ...); emitted as a 256-color SGR code.
/// - `0x100`: `terminal_default()`, the terminal's own default color. This
///   is `DEFAULT_COLOR`, which `tui::Style` also treats as "not set".
/// - `0x200`: `transparent()`.
/// - nonzero alpha byte (bits 24-31): true color, `r | g << 8 | b << 16`.
///   `Rgba8(r, g, b)` defaults alpha to 255.
///
/// Keep alpha nonzero for RGB values: `Rgba8(r, g, b, 0)` fails
/// `is_true_color()` and can collide with a palette index or sentinel.
struct Rgba8
{
    /// Packed representation; see the type description.
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

/// Text attribute bits (SGR bold, faint, italic, ...), combinable with `|`.
///
/// `Emphasis::none` is the empty set. Test membership with `has_emphasis`.
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

/// True when any bit of `flag` is set in `set`.
constexpr bool has_emphasis(Emphasis set, Emphasis flag) noexcept
{
    return (set & flag) != Emphasis::none;
}

/// Emphasis of a freshly cleared cell: none.
inline constexpr Emphasis DEFAULT_EMPHASIS = Emphasis::none;
/// Color of a freshly cleared cell: the terminal default.
inline constexpr Rgba8 DEFAULT_COLOR = Rgba8::terminal_default();

namespace tui {

/// Shared compositional style. Default channels inherit the parent;
/// explicitly set colors replace it and emphasis bits combine with `|`.
/// The terminal and graphical paths use the same value and operators.
struct Style
{
    Rgba8 fg = DEFAULT_COLOR;
    Rgba8 bg = DEFAULT_COLOR;
    Emphasis em = DEFAULT_EMPHASIS;

    constexpr Style operator|(const Style & other) const
    {
        return {
            other.fg != DEFAULT_COLOR ? other.fg : fg,
            other.bg != DEFAULT_COLOR ? other.bg : bg,
            em | other.em,
        };
    }
};

constexpr Style fg(Rgba8 color)
{
    return {color, DEFAULT_COLOR, DEFAULT_EMPHASIS};
}

constexpr Style bg(Rgba8 color)
{
    return {DEFAULT_COLOR, color, DEFAULT_EMPHASIS};
}

constexpr Style em(Emphasis emphasis)
{
    return {DEFAULT_COLOR, DEFAULT_COLOR, emphasis};
}

inline constexpr Style bold{DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::bold};
inline constexpr Style faint{DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::faint};
inline constexpr Style italic{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::italic};
inline constexpr Style underline{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::underline};
inline constexpr Style reverse{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::reverse};
inline constexpr Style strikethrough{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::strikethrough};

} // namespace tui

} // namespace nxtui
