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
    TerminalCompositor(nxt::Size size, GlyphTable & glyphs);
    void resize(nxt::Size size);

    Raster & back_buffer() noexcept;
    GlyphTable & glyphs() const noexcept;
    nxt::Size size() const noexcept;

    /// Set HUD height. In HUD mode, one separator row is reserved above the
    /// HUD and the scroll region ends above that separator. If the HUD plus
    /// separator cannot fit, the compositor uses full-screen mode.
    void set_hud_height(height_t hud_height, height_t term_height);
    void set_hud_height(
        height_t hud_height, height_t term_height, std::ostream & out);
    [[nodiscard]] height_t hud_height() const noexcept;

    // Public for testing the rendering pipeline without async runtime
    void present_frame();
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
