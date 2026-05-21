#include "nxt/compositor.hpp"
#include "nxt/ansi.hpp"
#include "nxt/raster-diff.hpp"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nxt::ui {
namespace {

[[nodiscard]] int row_index(const row_t row)
{
    return static_cast<int>((row - terminal_origin_v).count());
}

[[nodiscard]] int row_count(const height_t rows)
{
    return static_cast<int>(rows.count());
}

[[nodiscard]] height_t lines(const int count)
{
    return static_cast<std::size_t>(count) * ln;
}

[[nodiscard]] bool has_windowed_hud(
    const height_t hud_height, const height_t term_height)
{
    return hud_height > 0 * ln && hud_height < term_height;
}

[[nodiscard]] row_t hud_start_row_for(
    const height_t hud_height, const height_t term_height)
{
    if (has_windowed_hud(hud_height, term_height))
        return terminal_origin_v + (term_height - hud_height);
    if (hud_height > 0 * ln)
        return terminal_origin_v + 0 * ln;
    return terminal_origin_v + term_height;
}

[[nodiscard]] row_t scroll_bottom_for(
    const height_t hud_height, const height_t term_height)
{
    // Leave a 1-row quiet zone between the scroll region and the HUD.
    if (has_windowed_hud(hud_height, term_height))
        return hud_start_row_for(hud_height, term_height) - 2 * ln;
    return hud_start_row_for(hud_height, term_height) - 1 * ln;
}

[[nodiscard]] height_t raster_height_for(
    const height_t hud_height, const height_t term_height)
{
    if (hud_height == 0 * ln)
        return 0 * ln;
    if (has_windowed_hud(hud_height, term_height))
        return hud_height;
    return term_height;
}

[[nodiscard]] int chrome_start_row_for(
    const height_t hud_height, const height_t term_height)
{
    if (has_windowed_hud(hud_height, term_height))
        return row_index(hud_start_row_for(hud_height, term_height));
    if (hud_height > 0 * ln)
        return 0;
    return row_count(term_height);
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

struct HudGeometryChange
{
    height_t old_hud_height;
    height_t old_term_height;
    height_t new_hud_height;
    height_t new_term_height;
    bool old_windowed = false;
    bool new_windowed = false;
    int old_scroll_bottom = -1;
    int new_scroll_bottom = -1;
    bool initial_geometry = false;
};

HudGeometryChange describe_hud_change(
    height_t old_hud_height,
    height_t old_term_height,
    height_t new_hud_height,
    height_t new_term_height,
    bool geometry_initialized)
{
    auto old_windowed =
        has_windowed_hud(old_hud_height, old_term_height);
    auto new_windowed =
        has_windowed_hud(new_hud_height, new_term_height);
    return HudGeometryChange{
        .old_hud_height = old_hud_height,
        .old_term_height = old_term_height,
        .new_hud_height = new_hud_height,
        .new_term_height = new_term_height,
        .old_windowed = old_windowed,
        .new_windowed = new_windowed,
        .old_scroll_bottom =
            old_windowed
                ? row_index(
                    scroll_bottom_for(old_hud_height, old_term_height))
                : -1,
        .new_scroll_bottom =
            new_windowed
                ? row_index(
                    scroll_bottom_for(new_hud_height, new_term_height))
                : -1,
        .initial_geometry = !geometry_initialized,
    };
}

void reserve_scrollback_space_for_hud(
    ansi::Writer & wr,
    const HudGeometryChange & change)
{
    if (change.initial_geometry)
        return;

    if (!change.old_windowed || !change.new_windowed
        || change.new_scroll_bottom >= change.old_scroll_bottom)
        return;

    auto scroll_diff =
        change.old_scroll_bottom - change.new_scroll_bottom;
    wr.move_to(Pos::at(0 * ch, lines(change.old_scroll_bottom)));
    wr.scroll_up(lines(scroll_diff));
}

void apply_scroll_region(
    ansi::Writer & wr,
    const HudGeometryChange & change,
    row_t & hud_start_row)
{
    hud_start_row = hud_start_row_for(
        change.new_hud_height, change.new_term_height);

    if (!change.new_windowed) {
        wr.reset_scroll_region();
        return;
    }

    wr.set_scroll_region(
        terminal_origin_v + 0 * ln,
        scroll_bottom_for(change.new_hud_height, change.new_term_height));
}

void clear_chrome_rows(
    ansi::Writer & wr,
    const HudGeometryChange & change)
{
    if (change.initial_geometry)
        return;

    int clear_start = std::min(
        chrome_start_row_for(
            change.old_hud_height, change.old_term_height),
        chrome_start_row_for(
            change.new_hud_height, change.new_term_height));
    if (change.old_term_height == 0 * ln)
        clear_start = chrome_start_row_for(
            change.new_hud_height, change.new_term_height);
    clear_start =
        std::clamp(clear_start, 0, row_count(change.new_term_height));

    for (int row = clear_start; row < row_count(change.new_term_height);
         ++row) {
        wr.move_to(Pos::at(0 * ch, static_cast<std::size_t>(row) * ln));
        wr.clear_line();
    }
}

} // namespace

TerminalCompositor::TerminalCompositor(
    const nxt::Size size, GlyphTable & glyphs)
    : front_(size.w, size.h, glyphs)
    , back_(size.w, size.h, glyphs)
    , glyphs_(glyphs)
    , hud_height_(size.h)
    , term_height_(size.h)
    , hud_start_row_(terminal_origin_v + 0 * ln)
{
}

void TerminalCompositor::resize(nxt::Size size)
{
    // In HUD mode, the raster only covers HUD rows; fullscreen layouts use
    // the whole terminal.
    auto raster_h = raster_height_for(hud_height_, size.h);
    front_ = Raster(size.w, raster_h, glyphs_);
    back_ = Raster(size.w, raster_h, glyphs_);

    if (!geometry_initialized_)
        return;

    // Clear the compositor-owned region, preserving cursor position. The next
    // render will redraw the HUD at the resized width.
    std::string buf;
    ansi::Writer w(buf);
    w.save_cursor();

    if (has_windowed_hud(hud_height_, size.h)) {
        auto end_row = terminal_origin_v + size.h;
        auto start_row = hud_start_row_for(hud_height_, size.h);
        for (auto row = start_row; row < end_row; row += 1 * ln) {
            w.move_to(Pos{terminal_origin + 0 * ch, row});
            w.clear_line();
        }
    } else if (hud_height_ > 0 * ln) {
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
    set_hud_height(hud_height, term_height, std::cout);
}

void TerminalCompositor::set_hud_height(
    height_t hud_height, height_t term_height, std::ostream & out)
{
    auto new_hud_height = std::min(hud_height, term_height);
    if (new_hud_height == hud_height_ && term_height == term_height_)
        return;

    auto old_term_height = term_height_;
    auto old_hud_height = hud_height_;
    auto change = describe_hud_change(
        old_hud_height,
        old_term_height,
        new_hud_height,
        term_height,
        geometry_initialized_);

    hud_height_ = new_hud_height;
    term_height_ = term_height;

    // Calculate where the HUD starts
    // Note: DECSTBM (set scroll region) moves cursor to home, so
    // save/restore
    {
        std::string buf;
        ansi::Writer wr(buf);
        wr.save_cursor();
        wr.reset();

        reserve_scrollback_space_for_hud(wr, change);
        apply_scroll_region(wr, change, hud_start_row_);
        clear_chrome_rows(wr, change);

        wr.restore_cursor();
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.flush();
    }

    geometry_initialized_ = true;

    // Resize rasters to match HUD height
    auto raster_w = front_.width();
    auto raster_h = raster_height_for(new_hud_height, term_height);
    front_ = Raster(raster_w, raster_h, glyphs_);
    back_ = Raster(raster_w, raster_h, glyphs_);
}

height_t TerminalCompositor::hud_height() const noexcept
{
    return hud_height_;
}

int TerminalCompositor::scrollback_bottom_row() const noexcept
{
    return row_index(scroll_bottom_for(hud_height_, term_height_));
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

    // Offset for HUD mode: raster row 0 maps to hud_start_row_ on
    // terminal hud_start_row_ is a row_t (point), subtract origin to get
    // quantity offset
    auto row_offset = hud_start_row_ - terminal_origin_v;

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
