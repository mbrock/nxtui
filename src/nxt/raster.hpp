#pragma once

#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <experimental/mdspan>

#include "nxt/glyph-table.hpp"
#include "nxt/style.hpp"
#include "nxt/units.hpp"

namespace nxt {

/// Dynamic extents for terminal-cell rasters.
using mdspan_extents = std::experimental::
    extents<std::size_t, std::dynamic_extent, std::dynamic_extent>;
/// Mutable 2D glyph view.
using glyph_view_t =
    std::experimental::mdspan<
        GlyphTable::GlyphId,
        mdspan_extents,
        std::experimental::layout_stride>;
/// Const 2D glyph view.
using const_glyph_view_t =
    std::experimental::mdspan<
        const GlyphTable::GlyphId,
        mdspan_extents,
        std::experimental::layout_stride>;
/// Mutable 2D color view.
using color_view_t = std::experimental::
    mdspan<Rgba8, mdspan_extents, std::experimental::layout_stride>;
/// Const 2D color view.
using const_color_view_t =
    std::experimental::
        mdspan<const Rgba8, mdspan_extents, std::experimental::layout_stride>;
/// Mutable 2D emphasis view.
using emphasis_view_t = std::experimental::
    mdspan<Emphasis, mdspan_extents, std::experimental::layout_stride>;
/// Const 2D emphasis view.
using const_emphasis_view_t =
    std::experimental::mdspan<
        const Emphasis,
        mdspan_extents,
        std::experimental::layout_stride>;

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
    /// Column coordinate within the row.
    width_t col;
    /// Glyph id stored at the cell.
    GlyphTable::GlyphId glyph;
    /// Foreground color.
    Rgba8 fg;
    /// Background color.
    Rgba8 bg;
    /// Emphasis bits.
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

/// Cell data for inspection
struct Cell
{
    /// Glyph id stored at the cell.
    GlyphTable::GlyphId glyph;
    /// Foreground color.
    Rgba8 fg;
    /// Background color.
    Rgba8 bg;
    /// Emphasis bits.
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
