// PNG screenshots of TUI rasters / layouts.
//
// Renders the Raster's character grid as an image: one filled rectangle
// per cell for the background, then the cell's glyph drawn in the
// foreground colour.  Backed by cairo + pangocairo so we get
// system-fontconfig monospace text with proper UTF-8, combining marks,
// and emphasis (bold / italic / underline / reverse).
//
// Defaults to the dark Baltic palette so terminal-default colours and
// ANSI 0-15 cells get sensible RGB substitutions instead of being
// invisible.

#pragma once

#include <filesystem>
#include <string>

#include "nxtui/baltics.hpp"
#include "nxtui/raster.hpp"
#include "nxtui/tui.hpp"

namespace nxt::png {

struct Options
{
    /// Pango family name. "Berkeley Mono" is auto-loaded from a local
    /// ./fonts/ directory if present; otherwise fontconfig substitutes.
    std::string font_family = "Berkeley Mono";
    /// Font size in points.
    double font_size_pt = 12.0;

    /// Substituted for cells whose fg/bg is `terminal_default()`.
    Rgb8 default_fg = theme::baltic_church.fg;
    Rgb8 default_bg = theme::baltic_church.bg;

    /// Padding around the cell grid, in pixels.
    int padding_px = 12;
};

/// Write a Raster as a PNG.
void write(
    const Raster & raster,
    const std::filesystem::path & path,
    const Options & options = {});

/// Build a Raster from a Layout at the given cell size and write it.
template<tui::Layout L>
void write_layout(
    const L & layout,
    Size cells,
    const std::filesystem::path & path,
    const Options & options = {})
{
    GlyphTable glyphs;
    Raster raster(cells, glyphs);
    auto view = raster.view();
    layout.render(view, cells);
    write(raster, path, options);
}

}  // namespace nxt::png
