#pragma once

#include "nxtui/glyph-table.hpp"
#include "nxtui/raster.hpp"
#include "nxtui/regional-tty.hpp"
#include "nxtui/units.hpp"

#include <iosfwd>
#include <mutex>
#include <optional>

namespace nxtui::tui {

/// Puts rendered rasters on a terminal by writing only the cells that
/// changed since the last frame.
///
/// The compositor keeps two rasters. Render each frame into
/// `back_buffer()`, then call `present_frame`: it diffs the back buffer
/// against the front buffer (the last presented frame), writes the changed
/// runs with minimal SGR color/emphasis changes, wraps the output in
/// save/restore cursor, and then makes both buffers hold the new frame. The
/// back buffer therefore starts each frame with the previous frame's cells;
/// clear it first unless the new frame covers every cell.
///
/// The compositor owns the bottom of the terminal in one of three modes,
/// chosen by `set_hud_height`:
/// - full screen (the initial mode, or a HUD as tall as the terminal): the
///   raster covers the whole terminal;
/// - HUD (partial height): the raster is a fixed-height region at the
///   bottom, and a DECSTBM scroll region covers the rows above it, so
///   ordinary output written at the cursor scrolls there while the HUD
///   stays put (see `regional_tty::scrollback_append_state` for formatting
///   such output);
/// - hidden (height zero): no raster rows and no scroll region.
///
/// The compositor borrows `glyphs`, which must outlive it. It is not
/// thread-safe; overloads that write to `std::cout` can serialize with other
/// stdout writers through `set_output_mutex`. Output depends on
/// `ansi::mode`, so call `ansi::init()` or set the mode first.
///
/// @code
/// auto compositor = nxtui::tui::TerminalCompositor{term_size, glyphs};
/// compositor.set_hud_height(3 * nxtui::ln, term_size.h, std::cout);
/// auto & frame = compositor.back_buffer();
/// frame.clear();
/// auto view = frame.view();
/// layout.render(view, frame.extent());
/// compositor.present_frame(std::cout);
/// @endcode
class TerminalCompositor
{
public:
    /// Create blank buffers for a terminal of `size`, in full-screen mode.
    /// Nothing is written until `set_hud_height` or `present_frame`.
    TerminalCompositor(nxtui::Size size, GlyphTable & glyphs);
    /// Replace both buffers with blank rasters for a terminal of `size`.
    ///
    /// The raster height is computed from the current owned height (the
    /// HUD height, or the old terminal height in full-screen mode) clamped
    /// to the new terminal height, so a full-screen compositor does not
    /// grow with the terminal by itself. Once `set_hud_height` has
    /// installed a layout, it also clears the owned rows (or the whole
    /// screen) on `std::cout`. It does not update `partition()` or the
    /// terminal's scroll region: follow it with `set_hud_height` for the
    /// new terminal height.
    void resize(nxtui::Size size);

    /// Raster to render the next frame into. It holds the previously
    /// presented frame until you change it.
    Raster & back_buffer() noexcept;
    /// Shared glyph table used by both buffers.
    GlyphTable & glyphs() const noexcept;
    /// Size of the rasters: terminal width by HUD height in HUD mode, the
    /// full terminal in full-screen mode, zero rows when hidden.
    nxtui::Size size() const noexcept;

    /// Set the HUD height and repartition the terminal, writing the
    /// escapes to `std::cout` (under the output mutex, if set).
    ///
    /// A height of zero hides the HUD; a height at least `term_height`
    /// selects full-screen mode; anything between reserves that many rows
    /// at the bottom and sets the scroll region to the rows above. When the
    /// HUD grows or reappears, the content above it is scrolled up first so
    /// existing output moves above the HUD instead of being overwritten;
    /// when it is hidden, output is scrolled back down into the freed rows.
    /// If the partition is unchanged nothing is written. Otherwise both
    /// buffers are recreated blank at the new size.
    void set_hud_height(height_t hud_height, height_t term_height);
    /// `set_hud_height` writing to `out`, without taking the output mutex.
    void set_hud_height(
        height_t hud_height, height_t term_height, std::ostream & out);
    /// `set_hud_height` writing to `out`, with the terminal row of the
    /// cursor at first installation.
    ///
    /// `insertion_cursor` matters only for the first change that installs a
    /// HUD: if the cursor sits below the new scroll region, the terminal is
    /// scrolled up so the cursor's line, and output above it, end up above
    /// the HUD instead of under it. Without it the first installation
    /// scrolls nothing.
    void set_hud_height(
        height_t hud_height,
        height_t term_height,
        std::ostream & out,
        std::optional<row_t> insertion_cursor);
    /// Current HUD height.
    [[nodiscard]] height_t hud_height() const noexcept;
    /// Current terminal partition owned by the compositor.
    [[nodiscard]] const regional_tty::screen_partition &
    partition() const noexcept;
    /// Zero-based terminal row at the bottom of the scroll region in HUD
    /// mode, or -1 when there is no scroll region.
    [[nodiscard]] int scrollback_bottom_row() const noexcept;

    /// `present_frame(std::cout)` under the output mutex, if set.
    void present_frame();
    /// Write the cells that differ between the back and front buffers to
    /// `out` and flush, then make the back buffer the new front.
    ///
    /// Raster row 0 is placed at the top of the owned region (the HUD's
    /// first row, or terminal row 0 in full-screen mode). The cursor is
    /// saved and restored around the output.
    /// @throws std::logic_error if a changed cell's glyph contains a
    /// newline, carriage return, tab, vertical tab, form feed, or escape.
    void present_frame(std::ostream & out);

    /// Install a mutex that serializes this compositor's `std::cout` writes
    /// with other stdout writers. When set, `present_frame()`, `resize()`,
    /// and the two-argument `set_hud_height()` hold it around their write
    /// and flush; the overloads taking an `std::ostream &` never lock it, so
    /// a caller can hold the lock across its own output plus
    /// `present_frame(std::cout)`. The mutex is borrowed; null disables
    /// locking.
    void set_output_mutex(std::mutex * mutex) noexcept
    {
        output_mutex_ = mutex;
    }

private:
    Raster front_;
    Raster back_;
    GlyphTable & glyphs_;
    regional_tty::screen_partition partition_;
    bool geometry_initialized_ = false;
    // Non-owning; supplied by `set_output_mutex`. Null means callers
    // chose not to participate (tests with their own ostream).
    std::mutex * output_mutex_ = nullptr;
};

} // namespace nxtui::tui
