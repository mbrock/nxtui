#pragma once

#include "nxt/glyph-table.hpp"
#include "nxt/raster.hpp"
#include "nxt/units.hpp"

#include <iosfwd>

namespace nxt::ui {

/// Double-buffered terminal compositor with HUD/scroll region support.
class TerminalCompositor
{
public:
    /// Create double buffers for a terminal of `size`.
    TerminalCompositor(nxt::Size size, GlyphTable & glyphs);
    /// Resize both front and back buffers and terminal bookkeeping.
    void resize(nxt::Size size);

    /// Mutable buffer rendered by the next frame.
    Raster & back_buffer() noexcept;
    /// Shared glyph table used by both buffers.
    GlyphTable & glyphs() const noexcept;
    /// Current compositor size.
    nxt::Size size() const noexcept;

    /// Set HUD height. In HUD mode, the scroll region ends immediately above
    /// the HUD. If the HUD fills the terminal, the compositor uses full-screen
    /// mode.
    void set_hud_height(height_t hud_height, height_t term_height);
    void set_hud_height(
        height_t hud_height, height_t term_height, std::ostream & out);
    /// Current HUD height.
    [[nodiscard]] height_t hud_height() const noexcept;

    /// Present changed back-buffer cells to stdout.
    void present_frame();
    /// Present changed back-buffer cells to an output stream.
    void present_frame(std::ostream & out);

private:
    Raster front_;
    Raster back_;
    GlyphTable & glyphs_;
    height_t hud_height_{0 * ln};
    height_t term_height_{0 * ln};
    row_t hud_start_row_{
        terminal_origin_v + 0 * ln}; // row where HUD starts
};

} // namespace nxt::ui
