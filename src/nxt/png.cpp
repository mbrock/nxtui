#include "nxt/png.hpp"

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace nxt::png {

namespace {

// Register .ttf/.otf files in ./fonts/ with fontconfig the first time
// we render so locally-vendored fonts (e.g. Berkeley Mono) are
// discoverable by Pango without needing to be installed system-wide.
void register_local_fonts_once()
{
    static std::atomic<bool> done{false};
    if (done.exchange(true, std::memory_order_acq_rel))
        return;

    namespace fs = std::filesystem;
    fs::path dir{"fonts"};
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return;

    auto * config = FcConfigGetCurrent();
    for (auto & entry : fs::directory_iterator{dir, ec}) {
        if (!entry.is_regular_file())
            continue;
        auto ext = entry.path().extension().string();
        for (auto & c : ext) c = static_cast<char>(std::tolower(c));
        if (ext != ".ttf" && ext != ".otf")
            continue;
        auto path = entry.path().string();
        FcConfigAppFontAddFile(
            config, reinterpret_cast<const FcChar8 *>(path.c_str()));
    }
}

// ANSI 0-15 mapped onto baltic-church hues so palette colours render as
// something readable instead of black-on-black.
constexpr std::array<Rgb8, 16> ansi16 = {
    Rgb8{0x00, 0x00, 0x00},                                       // 0  black
    theme::baltic_church.coral,                                   // 1  red
    theme::baltic_church.green,                                   // 2  green
    theme::baltic_church.amber,                                   // 3  yellow
    theme::baltic_church.cyan_soft,                               // 4  blue
    theme::baltic_church.pink,                                    // 5  magenta
    theme::baltic_church.cyan,                                    // 6  cyan
    theme::baltic_church.fg,                                      // 7  white
    theme::baltic_church.fg_muted,                                // 8  br black
    theme::baltic_church.coral,                                   // 9  br red
    theme::baltic_church.green,                                   // 10 br green
    theme::baltic_church.amber,                                   // 11 br yellow
    theme::baltic_church.cyan,                                    // 12 br blue
    theme::baltic_church.pink,                                    // 13 br magenta
    theme::baltic_church.regex,                                   // 14 br cyan
    Rgb8{0xff, 0xff, 0xff},                                       // 15 br white
};

Rgb8 xterm256(std::uint8_t i)
{
    if (i < 16)
        return ansi16[i];
    if (i < 232) {
        const auto n = static_cast<int>(i - 16);
        constexpr std::array<std::uint8_t, 6> levels = {
            0, 95, 135, 175, 215, 255};
        return Rgb8{levels[(n / 36) % 6], levels[(n / 6) % 6], levels[n % 6]};
    }
    const auto v = static_cast<std::uint8_t>(8 + 10 * (i - 232));
    return Rgb8{v, v, v};
}

Rgb8 resolve_fg(Rgba8 c, const Options & opts)
{
    if (c.is_true_color())
        return c.to_rgb();
    if (c.is_palette())
        return xterm256(c.palette_index());
    return opts.default_fg;  // terminal_default / transparent
}

Rgb8 resolve_bg(Rgba8 c, const Options & opts)
{
    if (c.is_true_color())
        return c.to_rgb();
    if (c.is_palette())
        return xterm256(c.palette_index());
    return opts.default_bg;
}

void set_source_rgb(cairo_t * cr, Rgb8 c)
{
    cairo_set_source_rgb(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0);
}

// Block elements (U+2580..U+259F) are drawn as filled rectangles
// matching the cell, not as glyphs.  Pango-rasterised block chars leave
// thin seams between cells from font side-bearings + AA at small sizes;
// real terminals avoid this by painting these chars geometrically.
//
// Returns true if the codepoint was handled.
bool draw_block_element(
    cairo_t * cr,
    double x,
    double y,
    double w,
    double h,
    std::string_view bytes,
    Rgb8 color)
{
    if (bytes.size() != 3
        || static_cast<unsigned char>(bytes[0]) != 0xe2
        || static_cast<unsigned char>(bytes[1]) != 0x96)
        return false;

    const auto off =
        static_cast<unsigned>(static_cast<unsigned char>(bytes[2]) - 0x80);

    double rx = x;
    double ry = y;
    double rw = w;
    double rh = h;

    if (off == 0x00) {                            // ▀ top half
        rh = h / 2.0;
    } else if (off >= 0x01 && off <= 0x07) {      // ▁..▇ bottom n/8
        const auto f = off / 8.0;
        rh = h * f;
        ry = y + (h - rh);
    } else if (off == 0x08) {                     // █ full
        // already covers the cell
    } else if (off >= 0x09 && off <= 0x0f) {      // ▉..▏ left n/8
        const auto f = (16.0 - off) / 8.0;
        rw = w * f;
    } else if (off == 0x10) {                     // ▐ right half
        rw = w / 2.0;
        rx = x + w / 2.0;
    } else if (off == 0x14) {                     // ▔ top 1/8
        rh = h / 8.0;
    } else if (off == 0x15) {                     // ▕ right 1/8
        rw = w / 8.0;
        rx = x + (w - rw);
    } else {
        return false;  // shades, quadrants — fall through to Pango.
    }

    set_source_rgb(cr, color);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_fill(cr);
    return true;
}

struct PangoDescGuard
{
    PangoFontDescription * desc;
    ~PangoDescGuard() { pango_font_description_free(desc); }
};

struct LayoutGuard
{
    PangoLayout * layout;
    ~LayoutGuard() { g_object_unref(layout); }
};

struct AttrListGuard
{
    PangoAttrList * attrs;
    ~AttrListGuard()
    {
        if (attrs)
            pango_attr_list_unref(attrs);
    }
};

}  // namespace

void write(
    const Raster & raster,
    const std::filesystem::path & path,
    const Options & options)
{
    const auto cols = raster.width().count();
    const auto rows = raster.height().count();
    if (cols == 0 || rows == 0)
        throw std::invalid_argument{"png::write: empty raster"};

    register_local_fonts_once();

    // Use Pango's font metrics to size cells: approximate_char_width is
    // the actual monospace advance, and ascent+descent is the natural
    // line height.  These match what Pango will draw, so cell rects and
    // glyphs line up without horizontal seams.
    auto * probe_surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto * probe_cr = cairo_create(probe_surface);
    auto * probe_layout = pango_cairo_create_layout(probe_cr);

    auto desc_string = options.font_family + " "
        + std::to_string(options.font_size_pt);
    PangoDescGuard base_desc{
        pango_font_description_from_string(desc_string.c_str())};
    pango_layout_set_font_description(probe_layout, base_desc.desc);

    auto * probe_ctx = pango_layout_get_context(probe_layout);
    auto * metrics =
        pango_context_get_metrics(probe_ctx, base_desc.desc, nullptr);
    const int char_w_pango =
        pango_font_metrics_get_approximate_char_width(metrics);
    const int ascent_pango = pango_font_metrics_get_ascent(metrics);
    const int descent_pango = pango_font_metrics_get_descent(metrics);
    pango_font_metrics_unref(metrics);

    const int cell_w = std::max(1, char_w_pango / PANGO_SCALE);
    const int cell_h =
        std::max(1, (ascent_pango + descent_pango) / PANGO_SCALE);

    g_object_unref(probe_layout);
    cairo_destroy(probe_cr);
    cairo_surface_destroy(probe_surface);

    const int img_w = cell_w * static_cast<int>(cols) + 2 * options.padding_px;
    const int img_h = cell_h * static_cast<int>(rows) + 2 * options.padding_px;

    auto * surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img_w, img_h);
    auto * cr = cairo_create(surface);

    // Paint page background (acts as the gutter / padding fill).
    set_source_rgb(cr, options.default_bg);
    cairo_rectangle(cr, 0, 0, img_w, img_h);
    cairo_fill(cr);

    auto * layout = pango_cairo_create_layout(cr);

    // Pass 1: backgrounds.  Painting these first means glyphs sit on
    // top cleanly, and we don't have to worry about cairo composition.
    for (std::size_t y = 0; y < rows; ++y) {
        for (auto cell : raster.row(y * ln)) {
            auto fg = cell.fg;
            auto bg = cell.bg;
            if (has_emphasis(cell.em, Emphasis::reverse))
                std::swap(fg, bg);

            const auto px_x =
                options.padding_px + cell.col.count() * cell_w;
            const auto px_y =
                options.padding_px + static_cast<int>(y) * cell_h;
            set_source_rgb(cr, resolve_bg(bg, options));
            cairo_rectangle(cr, px_x, px_y, cell_w, cell_h);
            cairo_fill(cr);
        }
    }

    // Pass 2: glyphs.
    const auto & glyphs = raster.glyph_table();
    for (std::size_t y = 0; y < rows; ++y) {
        for (auto cell : raster.row(y * ln)) {
            auto bytes = glyphs[cell.glyph];
            // Skip blanks and wide-glyph continuation cells: nothing to draw,
            // and we already painted bg.
            if (bytes.empty() || (bytes.size() == 1 && bytes[0] == ' '))
                continue;

            auto fg = cell.fg;
            auto bg = cell.bg;
            if (has_emphasis(cell.em, Emphasis::reverse))
                std::swap(fg, bg);

            const auto px_x_block =
                options.padding_px + cell.col.count() * cell_w;
            const auto px_y_block =
                options.padding_px + static_cast<int>(y) * cell_h;
            if (draw_block_element(
                    cr,
                    px_x_block,
                    px_y_block,
                    cell_w,
                    cell_h,
                    bytes,
                    resolve_fg(fg, options)))
                continue;

            // Per-cell font description for bold/italic.
            auto * desc = pango_font_description_copy(base_desc.desc);
            if (has_emphasis(cell.em, Emphasis::bold))
                pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
            if (has_emphasis(cell.em, Emphasis::italic))
                pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);

            // Underline via Pango attributes.
            AttrListGuard attrs{pango_attr_list_new()};
            if (has_emphasis(cell.em, Emphasis::underline))
                pango_attr_list_insert(
                    attrs.attrs,
                    pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
            pango_layout_set_attributes(layout, attrs.attrs);

            pango_layout_set_text(
                layout, bytes.data(), static_cast<int>(bytes.size()));

            const auto px_x =
                options.padding_px + cell.col.count() * cell_w;
            const auto px_y =
                options.padding_px + static_cast<int>(y) * cell_h;
            cairo_move_to(cr, px_x, px_y);
            set_source_rgb(cr, resolve_fg(fg, options));
            pango_cairo_show_layout(cr, layout);
        }
    }

    g_object_unref(layout);
    cairo_destroy(cr);

    auto status = cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_destroy(surface);

    if (status != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error{
            std::string{"png::write: "} + cairo_status_to_string(status)};
}

}  // namespace nxt::png
