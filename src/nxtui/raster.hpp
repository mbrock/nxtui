#pragma once

#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <experimental/mdspan>

#include "nxtui/glyph-table.hpp"
#include "nxtui/style.hpp"
#include "nxtui/units.hpp"

namespace nxtui {

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

/// Copy of one raster cell, returned by `RasterView::get_cell`.
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

/// Borrowed, possibly strided window onto a raster's cells; the type every
/// layout renders into.
///
/// A cell holds a glyph id (from the shared `GlyphTable`), a foreground and
/// background `Rgba8`, and `Emphasis` bits. Positions are relative to the
/// view's own top-left corner, and every write outside the view is ignored,
/// so a layout can draw without bounds checks. `subraster` carves out a
/// child window for a nested layout.
///
/// The view is a cheap value holding pointers. It does not own the cells:
/// it is valid while the `Raster` it came from is alive and has not been
/// reassigned or resized, and the `GlyphTable` must outlive it. Writing
/// through a `const RasterView` is allowed; constness does not protect the
/// cells.
class RasterView
{
public:
    /// Wrap existing cell arrays. All four views must have the same
    /// extents. Most code gets views from `Raster::view()` instead.
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

    /// Width of the view in cells.
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

    /// View of the rectangle at `origin` with `size`, relative to this view.
    /// The rectangle is clipped to this view, so the result may be smaller
    /// than `size`, or empty.
    [[nodiscard]] RasterView
    subraster(Pos origin, Size size) const noexcept;

    /// Set the glyph id at `pos`. Out-of-bounds writes are ignored, as for
    /// the other setters.
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

    /// Write UTF-8 text on row `pos.y` starting at column `pos.x`, one
    /// grapheme cluster per glyph; returns the column after the last cell
    /// written.
    ///
    /// Only glyphs change; colors and emphasis are left as they are. A
    /// cluster `n` cells wide occupies its first cell plus `n - 1`
    /// continuation cells holding the empty glyph. Writing stops at the
    /// right edge, before a cluster that would not fit. Text is not
    /// filtered: control characters such as `\n` are stored as glyphs, and
    /// `tui::TerminalCompositor::present_frame` rejects them, so strip or
    /// split them first.
    col_t write_text(Pos pos, std::string_view text) const noexcept;

    /// Copy of the cell at `pos`, or `std::nullopt` if out of bounds.
    [[nodiscard]] std::optional<Cell> get_cell(Pos pos) const noexcept;

    /// Glyph ids as a `[row, column]` mdspan. The `*_2d` accessors give
    /// direct access to each channel.
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

    /// Glyph ids as a flat row-major range of references, e.g. for
    /// `std::ranges::fill`. `fgs()`, `bgs()`, and `ems()` do the same for
    /// the other channels.
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

    /// Glyph table used to intern text written through this view.
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

/// Owning grid of cells: glyph ids, foreground and background colors, and
/// emphasis, stored as four row-major arrays.
///
/// Render into it through `view()`. A raster is copyable (copying the cell
/// arrays, sharing the borrowed `GlyphTable`) and is what
/// `tui::TerminalCompositor` and `ansi::render_raster` consume. Its size is
/// fixed; resize by assigning a new raster, which invalidates outstanding
/// views.
class Raster
{
public:
    /// Allocate a `width` x `height` raster of spaces with `DEFAULT_COLOR`
    /// foreground and background and no emphasis. `glyphs` is borrowed and
    /// must outlive the raster and its views.
    Raster(std::size_t width, std::size_t height, GlyphTable & glyphs);
    Raster(width_t width, height_t height, GlyphTable & glyphs);
    Raster(Size size, GlyphTable & glyphs);

    /// View of the whole raster, valid until the raster is reassigned or
    /// destroyed.
    [[nodiscard]] RasterView view() noexcept;

    /// Implicit conversion to view (convenience)
    operator RasterView() noexcept
    {
        return view();
    }

    /// Width in cells.
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

    /// Reset every cell to a space with default colors and no emphasis.
    void clear();

    /// Row-major glyph storage, for diffing and tests.
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

    /// `len` glyph ids starting at column `x` of row `y`. No bounds check
    /// beyond `std::span::subspan`.
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

    /// Glyph table shared by this raster's cells.
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
/// Yields `(height_t y, zipped_row)` where `zipped_row` pairs corresponding
/// cells. Both rasters must have the same size and outlive the range.
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

} // namespace nxtui
