#include "nxt/compositor.hpp"
#include "nxt/ansi.hpp"
#include "nxt/raster-diff.hpp"

#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nxt::ui {
namespace {

namespace rtty = nxt::regional_tty;

[[nodiscard]] int row_index(const row_t row)
{
    return static_cast<int>((row - terminal_origin_v).count());
}

[[nodiscard]] rtty::screen_partition partition_for(
    const height_t hud_height, const height_t term_height)
{
    return rtty::screen_partition::for_bottom_fixed_height(
        term_height, hud_height);
}

[[nodiscard]] height_t raster_height_for(
    const rtty::screen_partition & partition)
{
    if (partition.hidden())
        return 0 * ln;
    if (partition.windowed())
        return partition.bottom_fixed.height();
    return partition.terminal.height();
}

void write_terminal_text(ansi::Writer & w, std::string_view text)
{
    for (auto ch : text) {
        switch (ch) {
        case '\n':
        case '\r':
        case '\t':
        case '\v':
        case '\f':
        case '\x1b':
            throw std::logic_error{
                "terminal raster contains control character glyph"};
        default:
            w.text(ch);
            break;
        }
    }
}

} // namespace

TerminalCompositor::TerminalCompositor(
    const nxt::Size size, GlyphTable & glyphs)
    : front_(size.w, size.h, glyphs)
    , back_(size.w, size.h, glyphs)
    , glyphs_(glyphs)
    , partition_(partition_for(size.h, size.h))
{
}

void TerminalCompositor::resize(nxt::Size size)
{
    // In HUD mode, the raster only covers HUD rows; fullscreen layouts use
    // the whole terminal.
    auto resized_partition =
        partition_for(partition_.bottom_fixed.height(), size.h);
    auto raster_h = raster_height_for(resized_partition);
    front_ = Raster(size.w, raster_h, glyphs_);
    back_ = Raster(size.w, raster_h, glyphs_);

    if (!geometry_initialized_)
        return;

    // Clear the compositor-owned region, preserving cursor position. The next
    // render will redraw the HUD at the resized width.
    std::string buf;
    ansi::Writer w(buf);
    w.save_cursor();

    if (resized_partition.windowed()) {
        for (auto row = resized_partition.bottom_fixed.top;
             row < resized_partition.bottom_fixed.bottom_exclusive;
             row += 1 * ln) {
            w.move_to(Pos{terminal_origin + 0 * ch, row});
            w.clear_line();
        }
    } else if (resized_partition.fullscreen()) {
        w.clear_screen();
    }

    w.restore_cursor();
    {
        // Serialize against UIRuntime::print/println etc.; the lock
        // is null in test/ostream-only contexts where the compositor
        // never sees stdout from another thread.
        auto guard = output_mutex_
                         ? std::unique_lock{*output_mutex_}
                         : std::unique_lock<std::mutex>{};
        std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::cout.flush();
    }
}

void TerminalCompositor::set_hud_height(
    height_t hud_height, height_t term_height)
{
    auto guard = output_mutex_ ? std::unique_lock{*output_mutex_}
                               : std::unique_lock<std::mutex>{};
    set_hud_height(hud_height, term_height, std::cout, std::nullopt);
}

void TerminalCompositor::set_hud_height(
    height_t hud_height, height_t term_height, std::ostream & out)
{
    set_hud_height(hud_height, term_height, out, std::nullopt);
}

void TerminalCompositor::set_hud_height(
    height_t hud_height,
    height_t term_height,
    std::ostream & out,
    std::optional<row_t> insertion_cursor)
{
    auto next_partition = partition_for(hud_height, term_height);
    if (next_partition == partition_)
        return;

    auto change = geometry_initialized_
                      ? rtty::repartition::from(partition_, next_partition)
                      : rtty::repartition::initial(
                            next_partition, insertion_cursor);

    {
        auto buf = rtty::emit_repartition<rtty::ansi_string_backend>(change);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.flush();
    }

    partition_ = next_partition;
    geometry_initialized_ = true;

    // Resize rasters to match HUD height
    auto raster_w = front_.width();
    auto raster_h = raster_height_for(partition_);
    front_ = Raster(raster_w, raster_h, glyphs_);
    back_ = Raster(raster_w, raster_h, glyphs_);
}

height_t TerminalCompositor::hud_height() const noexcept
{
    return partition_.bottom_fixed.height();
}

const regional_tty::screen_partition &
TerminalCompositor::partition() const noexcept
{
    return partition_;
}

int TerminalCompositor::scrollback_bottom_row() const noexcept
{
    if (!partition_.scroll)
        return -1;
    return row_index(partition_.scroll->bottom_margin());
}

Raster & TerminalCompositor::back_buffer() noexcept
{
    return back_;
}

GlyphTable & TerminalCompositor::glyphs() const noexcept
{
    return glyphs_;
}

nxt::Size TerminalCompositor::size() const noexcept
{
    return {back_.width(), back_.height()};
}

void TerminalCompositor::present_frame()
{
    auto guard = output_mutex_ ? std::unique_lock{*output_mutex_}
                               : std::unique_lock<std::mutex>{};
    present_frame(std::cout);
}

void TerminalCompositor::present_frame(std::ostream & out)
{
    std::string buf;
    ansi::Writer w(buf);

    // Save cursor so HUD rendering doesn't disturb log output position
    w.save_cursor();

    // Offset for HUD mode: raster row 0 maps to the partition chrome start.
    auto row_offset = partition_.chrome_start() - terminal_origin_v;

    // Track current colors to re-emit after SGR reset
    std::optional<Rgba8> current_fg;
    std::optional<Rgba8> current_bg;

    // Helper to emit color (palette or true color)
    auto emit_fg = [&w](const Rgba8 & c) {
        if (c.is_palette())
            w.fg_palette(c.palette_index());
        else
            w.fg(c.to_rgb());
    };

    auto emit_bg = [&w](const Rgba8 & c) {
        if (c.is_palette())
            w.bg_palette(c.palette_index());
        else
            w.bg(c.to_rgb());
    };

    diff_rasters(front_, back_, [&](const ChangeRun & run) {
        // Offset position to HUD region
        auto pos = Pos{run.origin.x, run.origin.y + row_offset};
        w.move_to(pos);

        // Handle emphasis reset first (SGR 0 resets everything)
        if (run.em_reset) {
            w.reset();
            // Re-emit colors after reset
            if (current_fg)
                emit_fg(*current_fg);
            if (current_bg)
                emit_bg(*current_bg);
        }

        // Update background
        if (run.bg_reset) {
            w.bg_default();
            current_bg = std::nullopt;
        } else if (run.bg_change) {
            emit_bg(*run.bg_change);
            current_bg = run.bg_change;
        }

        // Update foreground
        if (run.fg_reset) {
            w.fg_default();
            current_fg = std::nullopt;
        } else if (run.fg_change) {
            emit_fg(*run.fg_change);
            current_fg = run.fg_change;
        }

        // Apply new emphasis.
        if (run.em_change)
            w.style(*run.em_change);

        for (const auto gid : run.glyphs)
            if (auto text = glyphs_.get(gid))
                write_terminal_text(w, *text);
    });

    // Restore cursor to where it was before HUD render
    w.restore_cursor();

    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    out.flush();

    std::swap(front_, back_);

    back_ = front_;
}

} // namespace nxt::ui
