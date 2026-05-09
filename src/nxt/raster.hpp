#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <experimental/mdspan>

#include "nxt/glyph-table.hpp"
#include "nxt/units.hpp"

namespace nxt {

struct Rgb8
{
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

/// Terminal color: packed into 32 bits.
///
/// Encoding (uses alpha=0 space for special values):
///   alpha > 0:           True color RGBA (24-bit color + alpha)
///   0x00000000-0x000000FF: 256-color palette (includes ANSI 16)
///   0x00000100:          Terminal default (SGR 39/49)
///
/// This lets us represent all terminal color modes in a single
/// uint32_t:
///   - ANSI 16 colors (palette 0-15, respects terminal theme)
///   - 256-color palette (0-255)
///   - 24-bit true color with alpha
///   - Terminal default (reset to user's configured color)
///
struct Rgba8
{
    std::uint32_t value;

    // ==========================================================================
    // Construction
    // ==========================================================================

    /// Construct from RGBA components (true color)
    constexpr Rgba8(
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b,
        std::uint8_t a = 255) noexcept
        : value(r | (g << 8) | (b << 16) | (a << 24))
    {
    }

    /// Construct from RGB components (opaque true color)
    constexpr Rgba8(Rgb8 rgb, std::uint8_t a = 255) noexcept
        : value(rgb.r | (rgb.g << 8) | (rgb.b << 16) | (a << 24))
    {
    }

    /// Private: construct from raw value
    static constexpr Rgba8 from_raw(std::uint32_t v) noexcept
    {
        Rgba8 c{0, 0, 0, 0};
        c.value = v;
        return c;
    }

    // ==========================================================================
    // Named constructors for special values
    // ==========================================================================

    /// Terminal default color (SGR 39 for fg, SGR 49 for bg)
    static constexpr Rgba8 terminal_default() noexcept
    {
        return from_raw(0x00000100);
    }

    /// 256-color palette (0-255, includes ANSI 16 at 0-15)
    static constexpr Rgba8 palette(std::uint8_t index) noexcept
    {
        return from_raw(index);
    }

    /// ANSI colors by name (0-15)
    static constexpr Rgba8 black() noexcept
    {
        return palette(0);
    }

    static constexpr Rgba8 red() noexcept
    {
        return palette(1);
    }

    static constexpr Rgba8 green() noexcept
    {
        return palette(2);
    }

    static constexpr Rgba8 yellow() noexcept
    {
        return palette(3);
    }

    static constexpr Rgba8 blue() noexcept
    {
        return palette(4);
    }

    static constexpr Rgba8 magenta() noexcept
    {
        return palette(5);
    }

    static constexpr Rgba8 cyan() noexcept
    {
        return palette(6);
    }

    static constexpr Rgba8 white() noexcept
    {
        return palette(7);
    }

    static constexpr Rgba8 bright_black() noexcept
    {
        return palette(8);
    }

    static constexpr Rgba8 bright_red() noexcept
    {
        return palette(9);
    }

    static constexpr Rgba8 bright_green() noexcept
    {
        return palette(10);
    }

    static constexpr Rgba8 bright_yellow() noexcept
    {
        return palette(11);
    }

    static constexpr Rgba8 bright_blue() noexcept
    {
        return palette(12);
    }

    static constexpr Rgba8 bright_magenta() noexcept
    {
        return palette(13);
    }

    static constexpr Rgba8 bright_cyan() noexcept
    {
        return palette(14);
    }

    static constexpr Rgba8 bright_white() noexcept
    {
        return palette(15);
    }

    /// Fully transparent (for compositing - layer below shows through)
    static constexpr Rgba8 transparent() noexcept
    {
        // Use a value outside palette and default range
        return from_raw(0x00000200);
    }

    // ==========================================================================
    // Queries
    // ==========================================================================

    [[nodiscard]] constexpr bool is_true_color() const noexcept
    {
        return (value >> 24) > 0; // alpha > 0
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

    // ==========================================================================
    // RGBA component access (only meaningful for true color)
    // ==========================================================================

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

    /// Convert to RGB (only meaningful for true color)
    [[nodiscard]] constexpr Rgb8 to_rgb() const noexcept
    {
        return Rgb8{r(), g(), b()};
    }

    // ==========================================================================
    // Comparison and formatting
    // ==========================================================================

    constexpr auto operator<=>(const Rgba8 &) const = default;

    friend std::ostream & operator<<(std::ostream & os, const Rgba8 & c);
};

// ============================================================================
// Text emphasis (SGR attributes beyond color)
// ============================================================================

/// Bitfield for text emphasis attributes.
/// These map directly to SGR codes: bold=1, faint=2, italic=3, etc.
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

/// Default emphasis (none)
inline constexpr Emphasis DEFAULT_EMPHASIS = Emphasis::none;

// ============================================================================
// Type aliases for mdspan views
// ============================================================================

using mdspan_extents = std::experimental::
    extents<std::size_t, std::dynamic_extent, std::dynamic_extent>;
using glyph_view_t =
    std::experimental::mdspan<
        GlyphTable::GlyphId,
        mdspan_extents,
        std::experimental::layout_stride>;
using const_glyph_view_t =
    std::experimental::mdspan<
        const GlyphTable::GlyphId,
        mdspan_extents,
        std::experimental::layout_stride>;
using color_view_t = std::experimental::
    mdspan<Rgba8, mdspan_extents, std::experimental::layout_stride>;
using const_color_view_t =
    std::experimental::
        mdspan<const Rgba8, mdspan_extents, std::experimental::layout_stride>;
using emphasis_view_t = std::experimental::
    mdspan<Emphasis, mdspan_extents, std::experimental::layout_stride>;
using const_emphasis_view_t =
    std::experimental::mdspan<
        const Emphasis,
        mdspan_extents,
        std::experimental::layout_stride>;

/// Default color for terminal cells (resets to terminal's configured
/// color)
inline constexpr Rgba8 DEFAULT_COLOR = Rgba8::terminal_default();

// ============================================================================
// mdspan to range adapter
// ============================================================================

/// Convert a 2D mdspan to a flat range (row-major order).
/// Works with any layout (contiguous or strided from submdspan).
template<typename T, typename Extents, typename Layout, typename Accessor>
auto as_range(std::experimental::mdspan<T, Extents, Layout, Accessor> m)
{
    const auto rows = m.extent(0);
    const auto cols = m.extent(1);
    return std::views::iota(std::size_t{0}, rows * cols)
           | std::views::transform(
               [=](std::size_t i) -> T & { return m[i / cols, i % cols]; });
}

/// Get a single row from a 2D mdspan as a range.
template<typename T, typename Extents, typename Layout, typename Accessor>
auto row_range(
    std::experimental::mdspan<T, Extents, Layout, Accessor> m,
    std::size_t row_idx)
{
    const auto cols = m.extent(1);
    return std::views::iota(std::size_t{0}, cols)
           | std::views::transform(
               [=](std::size_t col) -> T & { return m[row_idx, col]; });
}

/// Get an indexed row range (pairs of column index and value
/// reference).
template<typename T, typename Extents, typename Layout, typename Accessor>
auto indexed_row(
    std::experimental::mdspan<T, Extents, Layout, Accessor> m,
    std::size_t row_idx)
{
    const auto cols = m.extent(1);
    return std::views::iota(std::size_t{0}, cols)
           | std::views::transform([=](std::size_t col) {
                 return std::pair<std::size_t, T &>{col, m[row_idx, col]};
             });
}

/// A cell with its column position for iteration.
struct IndexedCell
{
    width_t col;
    GlyphTable::GlyphId glyph;
    Rgba8 fg;
    Rgba8 bg;
    Emphasis em;

    bool operator==(const IndexedCell & other) const
    {
        return glyph == other.glyph && fg == other.fg && bg == other.bg
               && em == other.em;
    }
};

/// Get a row as indexed cells (col, glyph, fg, bg, em).
inline auto indexed_cell_row(
    const_glyph_view_t glyphs,
    const_color_view_t fgs,
    const_color_view_t bgs,
    const_emphasis_view_t ems,
    std::size_t row_idx)
{
    const auto cols = glyphs.extent(1);
    return std::views::iota(std::size_t{0}, cols)
           | std::views::transform([=](std::size_t x) {
                 return IndexedCell{
                     x * ch,
                     glyphs[row_idx, x],
                     fgs[row_idx, x],
                     bgs[row_idx, x],
                     ems[row_idx, x]};
             });
}

// ============================================================================
// RasterView - non-owning view into raster storage
// ============================================================================

/// Cell data for inspection
struct Cell
{
    GlyphTable::GlyphId glyph;
    Rgba8 fg;
    Rgba8 bg;
    Emphasis em;
};

/// Non-owning view into raster storage. This is the primary working
/// type for all rendering operations. Views can create sub-views
/// (subraster) for hierarchical layout.
class RasterView
{
public:
    /// Construct from mdspan views and glyph table
    RasterView(
        glyph_view_t glyphs,
        color_view_t fgs,
        color_view_t bgs,
        emphasis_view_t ems,
        GlyphTable & glyph_table) noexcept
        : glyphs_(glyphs)
        , fgs_(fgs)
        , bgs_(bgs)
        , ems_(ems)
        , glyph_table_(&glyph_table)
    {
    }

    /// Dimensions
    [[nodiscard]] width_t width() const noexcept
    {
        return glyphs_.extent(1) * ch;
    }

    [[nodiscard]] height_t height() const noexcept
    {
        return glyphs_.extent(0) * ln;
    }

    [[nodiscard]] Size extent() const noexcept
    {
        return {width(), height()};
    }

    /// Create a sub-view of a rectangular region.
    /// Coordinates are relative to this view.
    [[nodiscard]] RasterView
    subraster(Pos origin, Size size) const noexcept;

    /// Set glyph at position. Silently ignores out-of-bounds.
    void set_glyph(Pos pos, GlyphTable::GlyphId gid) const noexcept;

    /// Set foreground color at position
    void set_fg(Pos pos, Rgba8 color) const noexcept;

    /// Set background color at position
    void set_bg(Pos pos, Rgba8 color) const noexcept;

    /// Set emphasis at position
    void set_em(Pos pos, Emphasis em) const noexcept;

    /// Convenience: set ASCII character
    void set_char(Pos pos, char c) const noexcept
    {
        set_glyph(pos, static_cast<GlyphTable::GlyphId>(c));
    }

    /// Write UTF-8 text. Returns ending column position.
    col_t write_text(Pos pos, std::string_view text) const noexcept;

    /// Get cell data. Returns nullopt if out of bounds.
    [[nodiscard]] std::optional<Cell> get_cell(Pos pos) const noexcept;

    /// 2D mdspan views for direct access
    [[nodiscard]] glyph_view_t glyphs_2d() const noexcept
    {
        return glyphs_;
    }

    [[nodiscard]] color_view_t fgs_2d() const noexcept
    {
        return fgs_;
    }

    [[nodiscard]] color_view_t bgs_2d() const noexcept
    {
        return bgs_;
    }

    [[nodiscard]] emphasis_view_t ems_2d() const noexcept
    {
        return ems_;
    }

    /// Flat ranges for algorithms (row-major order)
    [[nodiscard]] auto glyphs() const
    {
        return as_range(glyphs_);
    }

    [[nodiscard]] auto fgs() const
    {
        return as_range(fgs_);
    }

    [[nodiscard]] auto bgs() const
    {
        return as_range(bgs_);
    }

    [[nodiscard]] auto ems() const
    {
        return as_range(ems_);
    }

    /// Access glyph table
    [[nodiscard]] GlyphTable & glyph_table() const noexcept
    {
        return *glyph_table_;
    }

private:
    glyph_view_t glyphs_;
    color_view_t fgs_;
    color_view_t bgs_;
    emphasis_view_t ems_;
    GlyphTable * glyph_table_;
};

// ============================================================================
// Raster - owning storage that produces views
// ============================================================================

/// Owning raster storage. Allocates and manages the underlying arrays.
/// Use view() to get a RasterView for rendering operations.
class Raster
{
public:
    /// Initialize with given dimensions.
    /// All cells default to space (ASCII 32) with DEFAULT_COLOR.
    Raster(std::size_t width, std::size_t height, GlyphTable & glyphs);
    Raster(width_t width, height_t height, GlyphTable & glyphs);
    Raster(Size size, GlyphTable & glyphs);

    /// Get a view of the entire raster
    [[nodiscard]] RasterView view() noexcept;

    /// Implicit conversion to view (convenience)
    operator RasterView() noexcept
    {
        return view();
    }

    /// Dimensions
    [[nodiscard]] width_t width() const noexcept
    {
        return width_;
    }

    [[nodiscard]] height_t height() const noexcept
    {
        return height_;
    }

    [[nodiscard]] Size extent() const noexcept
    {
        return {width_, height_};
    }

    /// Clear to spaces with default colors
    void clear();

    /// Direct access to storage (for diffing)
    [[nodiscard]] std::span<const GlyphTable::GlyphId>
    glyphs() const noexcept
    {
        return glyphs_storage_;
    }

    [[nodiscard]] std::span<const Rgba8> fgs() const noexcept
    {
        return fgs_storage_;
    }

    [[nodiscard]] std::span<const Rgba8> bgs() const noexcept
    {
        return bgs_storage_;
    }

    [[nodiscard]] std::span<const Emphasis> ems() const noexcept
    {
        return ems_storage_;
    }

    /// Get a span of glyphs for a region on a row
    [[nodiscard]] std::span<const GlyphTable::GlyphId>
    glyph_span(height_t y, width_t x, std::size_t len) const noexcept
    {
        const auto cols = width_.count();
        const auto offset =
            y.count() * cols + x.count();
        return std::span{glyphs_storage_}.subspan(offset, len);
    }

    /// 2D views (const, for diffing)
    [[nodiscard]] const_glyph_view_t glyphs_2d() const noexcept;
    [[nodiscard]] const_color_view_t fgs_2d() const noexcept;
    [[nodiscard]] const_color_view_t bgs_2d() const noexcept;
    [[nodiscard]] const_emphasis_view_t ems_2d() const noexcept;

    /// Get a row as indexed cells for iteration
    [[nodiscard]] auto row(height_t y) const
    {
        return indexed_cell_row(
            glyphs_2d(),
            fgs_2d(),
            bgs_2d(),
            ems_2d(),
            y.count());
    }

    /// Iterate rows (just the row ranges)
    [[nodiscard]] auto rows() const
    {
        return std::views::iota(
                   std::size_t{0}, height_.count())
               | std::views::transform(
                   [this](std::size_t y) { return row(y * ln); });
    }

    /// Iterate rows with their y coordinate: (height_t, row_range)
    [[nodiscard]] auto indexed_rows() const
    {
        return std::views::iota(
                   std::size_t{0}, height_.count())
               | std::views::transform([this](std::size_t yi) {
                     const auto y = yi * ln;
                     return std::pair{y, row(y)};
                 });
    }

    /// Access glyph table
    [[nodiscard]] GlyphTable & glyph_table() const noexcept
    {
        return *glyph_table_;
    }

private:
    width_t width_;
    height_t height_;
    std::vector<GlyphTable::GlyphId> glyphs_storage_;
    std::vector<Rgba8> fgs_storage_;
    std::vector<Rgba8> bgs_storage_;
    std::vector<Emphasis> ems_storage_;
    GlyphTable * glyph_table_;
};

// ============================================================================
// Row-based iteration helpers
// ============================================================================

/// Zip two rasters' rows together for comparison.
/// Yields (height_t y, zipped_row) where zipped_row pairs corresponding
/// cells.
inline auto zip_rows(const Raster & front, const Raster & back)
{
    return std::views::iota(
               std::size_t{0}, back.height().count())
           | std::views::transform([&](std::size_t yi) {
                 const auto y = yi * ln;
                 return std::pair{
                     y, std::views::zip(front.row(y), back.row(y))};
             });
}

} // namespace nxt
