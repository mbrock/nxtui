#include <algorithm>
#include <array>
#include <iomanip>

#include <experimental/mdspan>

#include "nxt/raster.hpp"
#include "nxt/utf8.hpp"

namespace nxt {

namespace {

inline std::size_t cols_from(width_t w)
{
    return w.count();
}

inline std::size_t rows_from(height_t h)
{
    return h.count();
}

auto row_major_mapping(const std::size_t rows, const std::size_t cols)
{
    return std::experimental::layout_stride::mapping<mdspan_extents>(
        mdspan_extents{rows, cols}, std::array<std::size_t, 2>{cols, 1});
}

} // namespace

std::ostream & operator<<(std::ostream & os, const Rgba8 & c)
{
    auto flags = os.flags();
    auto fill = os.fill();
    os << "rgba8(" << std::hex << std::setfill('0')
       << std::setw(2) << static_cast<int>(c.r()) << ','
       << std::setw(2) << static_cast<int>(c.g()) << ','
       << std::setw(2) << static_cast<int>(c.b()) << ','
       << std::setw(2) << static_cast<int>(c.a()) << ')';
    os.flags(flags);
    os.fill(fill);
    return os;
}

void RasterView::set_glyph(
    const Pos pos, const GlyphTable::GlyphId gid) const noexcept
{
    const auto x = pos.col();
    const auto y = pos.row();
    if (x >= glyphs_.extent(1) || y >= glyphs_.extent(0))
        return;

    glyphs_[y, x] = gid;
}

void RasterView::set_fg(const Pos pos, const Rgba8 color) const noexcept
{
    const auto x = pos.col();
    const auto y = pos.row();
    if (x >= glyphs_.extent(1) || y >= glyphs_.extent(0))
        return;

    fgs_[y, x] = color;
}

void RasterView::set_bg(const Pos pos, const Rgba8 color) const noexcept
{
    const auto x = pos.col();
    const auto y = pos.row();
    if (x >= glyphs_.extent(1) || y >= glyphs_.extent(0))
        return;

    bgs_[y, x] = color;
}

void RasterView::set_em(const Pos pos, const Emphasis em) const noexcept
{
    const auto x = pos.col();
    const auto y = pos.row();
    if (x >= glyphs_.extent(1) || y >= glyphs_.extent(0))
        return;

    ems_[y, x] = em;
}

col_t RasterView::write_text(
    const Pos pos, const std::string_view text) const noexcept
{
    const auto rows = glyphs_.extent(0);
    const auto cols = glyphs_.extent(1);
    const auto y = pos.row();

    if (y >= rows)
        return pos.x;

    std::size_t col = pos.col();
    auto i = utf8::byte_offset(0);

    while (i.count() < text.size() && col < cols) {
        const auto cluster_end = utf8::next(text, i);
        const std::string_view glyph_bytes =
            text.substr(i.count(), cluster_end - i);
        const auto cell_width = utf8::cluster_width(glyph_bytes);
        if (cell_width > (cols - col) * ch)
            break;

        const GlyphTable::GlyphId gid = glyph_table_->intern(glyph_bytes);

        glyphs_[y, col] = gid;
        // A wide grapheme occupies one printable cell plus empty continuation
        // cells. Diff/render code can then stream each glyph id once without
        // having to rediscover UTF-8 width later.
        for (std::size_t dx = 1; dx < cell_width.count(); ++dx)
            glyphs_[y, col + dx] = glyph_table_->intern("");

        i = cluster_end;
        col += cell_width.count();
    }

    return terminal_origin + col * ch;
}

std::optional<Cell> RasterView::get_cell(const Pos pos) const noexcept
{
    const auto x = pos.col();
    const auto y = pos.row();
    if (x >= glyphs_.extent(1) || y >= glyphs_.extent(0))
        return std::nullopt;

    return Cell{glyphs_[y, x], fgs_[y, x], bgs_[y, x], ems_[y, x]};
}

RasterView
RasterView::subraster(const Pos origin, const Size size) const noexcept
{
    using std::experimental::submdspan;

    const std::size_t x = origin.col();
    const std::size_t y = origin.row();
    const std::size_t w = cols_from(size.w);
    const std::size_t h = rows_from(size.h);

    const std::size_t x0 = std::min(x, glyphs_.extent(1));
    const std::size_t y0 = std::min(y, glyphs_.extent(0));
    const std::size_t x1 = std::min(x + w, glyphs_.extent(1));
    const std::size_t y1 = std::min(y + h, glyphs_.extent(0));

    const auto glyph_sub =
        submdspan(glyphs_, std::pair{y0, y1}, std::pair{x0, x1});
    const auto fg_sub =
        submdspan(fgs_, std::pair{y0, y1}, std::pair{x0, x1});
    const auto bg_sub =
        submdspan(bgs_, std::pair{y0, y1}, std::pair{x0, x1});
    const auto em_sub =
        submdspan(ems_, std::pair{y0, y1}, std::pair{x0, x1});

    return RasterView(glyph_sub, fg_sub, bg_sub, em_sub, *glyph_table_);
}

Raster::Raster(
    const std::size_t width, const std::size_t height, GlyphTable & glyphs)
    : Raster(width * ch, height * ln, glyphs)
{
}

Raster::Raster(
    const width_t width, const height_t height, GlyphTable & glyphs)
    : width_(width)
    , height_(height)
    , glyphs_storage_(cols_from(width) * rows_from(height), 32)
    , fgs_storage_(cols_from(width) * rows_from(height), DEFAULT_COLOR)
    , bgs_storage_(cols_from(width) * rows_from(height), DEFAULT_COLOR)
    , ems_storage_(cols_from(width) * rows_from(height), DEFAULT_EMPHASIS)
    , glyph_table_(&glyphs)
{
}

Raster::Raster(const Size size, GlyphTable & glyphs)
    : Raster(size.w, size.h, glyphs)
{
}

RasterView Raster::view() noexcept
{
    const auto rows = rows_from(height_);
    const auto cols = cols_from(width_);
    const auto mapping = row_major_mapping(rows, cols);

    glyph_view_t glyphs(glyphs_storage_.data(), mapping);
    color_view_t fgs(fgs_storage_.data(), mapping);
    color_view_t bgs(bgs_storage_.data(), mapping);
    emphasis_view_t ems(ems_storage_.data(), mapping);

    return RasterView(glyphs, fgs, bgs, ems, *glyph_table_);
}

void Raster::clear()
{
    std::ranges::fill(glyphs_storage_, 32);
    std::ranges::fill(fgs_storage_, DEFAULT_COLOR);
    std::ranges::fill(bgs_storage_, DEFAULT_COLOR);
    std::ranges::fill(ems_storage_, DEFAULT_EMPHASIS);
}

const_glyph_view_t Raster::glyphs_2d() const noexcept
{
    const auto rows = rows_from(height_);
    const auto cols = cols_from(width_);
    return const_glyph_view_t(
        glyphs_storage_.data(), row_major_mapping(rows, cols));
}

const_color_view_t Raster::fgs_2d() const noexcept
{
    const auto rows = rows_from(height_);
    const auto cols = cols_from(width_);
    return const_color_view_t(
        fgs_storage_.data(), row_major_mapping(rows, cols));
}

const_color_view_t Raster::bgs_2d() const noexcept
{
    const auto rows = rows_from(height_);
    const auto cols = cols_from(width_);
    return const_color_view_t(
        bgs_storage_.data(), row_major_mapping(rows, cols));
}

const_emphasis_view_t Raster::ems_2d() const noexcept
{
    const auto rows = rows_from(height_);
    const auto cols = cols_from(width_);
    return const_emphasis_view_t(
        ems_storage_.data(), row_major_mapping(rows, cols));
}

} // namespace nxt
