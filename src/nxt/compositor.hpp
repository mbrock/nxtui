#pragma once

#include "nxt/glyph-table.hpp"
#include "nxt/raster.hpp"
#include "nxt/regional-tty.hpp"
#include "nxt/units.hpp"

#include <iosfwd>
#include <mutex>
#include <optional>

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
    /// Bottom row used for scrollback output in windowed HUD mode.
    [[nodiscard]] int scrollback_bottom_row() const noexcept;

    /// Present changed back-buffer cells to stdout.
    void present_frame();
    /// Present changed back-buffer cells to an output stream.
    void present_frame(std::ostream & out);

    /// Install the runtime's stdout serialization mutex. When set, the
    /// compositor's `present_frame()`, `resize()`, and `set_hud_height()`
    /// overloads that write to `std::cout` acquire this lock around their
    /// write+flush. The runtime render loop uses the ostream-taking
    /// `present_frame(std::cout)` while already holding the same lock so queued
    /// scrollback output and HUD diffs are emitted as one presentation step.
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

} // namespace nxt::ui
