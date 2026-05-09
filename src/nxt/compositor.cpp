#include "nxt/compositor.hpp"
#include "nxt/ansi.hpp"
#include "nxt/raster-diff.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace nxt::ui {
namespace {

constexpr auto separator_height = 1 * ln;
constexpr std::string_view separator_glyph = "▔";

[[nodiscard]] int row_index(const row_t row)
{
    return static_cast<int>(
        (row - terminal_origin_v).count());
}

[[nodiscard]] int row_count(const height_t rows)
{
    return static_cast<int>(rows.count());
}

[[nodiscard]] height_t lines(const int count)
{
    return static_cast<std::size_t>(count) * ln;
}

[[nodiscard]] bool has_separator_hud(
    const height_t hud_height, const height_t term_height)
{
    return hud_height > 0 * ln
        && hud_height + separator_height < term_height;
}

[[nodiscard]] row_t hud_start_row_for(
    const height_t hud_height, const height_t term_height)
{
    if (has_separator_hud(hud_height, term_height))
        return terminal_origin_v + (term_height - hud_height);
    if (hud_height > 0 * ln)
        return terminal_origin_v + 0 * ln;
    return terminal_origin_v + term_height;
}

[[nodiscard]] row_t separator_row_for(
    const height_t hud_height, const height_t term_height)
{
    return hud_start_row_for(hud_height, term_height) - separator_height;
}

[[nodiscard]] row_t scroll_bottom_for(
    const height_t hud_height, const height_t term_height)
{
    return separator_row_for(hud_height, term_height) - 1 * ln;
}

[[nodiscard]] height_t raster_height_for(
    const height_t hud_height, const height_t term_height)
{
    if (hud_height == 0 * ln)
        return 0 * ln;
    if (has_separator_hud(hud_height, term_height))
        return hud_height;
    return term_height;
}

[[nodiscard]] int chrome_start_row_for(
    const height_t hud_height, const height_t term_height)
{
    if (has_separator_hud(hud_height, term_height))
        return row_index(separator_row_for(hud_height, term_height));
    if (hud_height > 0 * ln)
        return 0;
    return row_count(term_height);
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

    // Clear the compositor-owned region, preserving cursor position. The next
    // render will redraw the separator at the resized width.
    std::string buf;
    ansi::Writer w(buf);
    w.save_cursor();

    if (has_separator_hud(hud_height_, size.h)) {
        auto end_row = terminal_origin_v + size.h;
        auto start_row = separator_row_for(hud_height_, size.h);
        for (auto row = start_row; row < end_row; row += 1 * ln) {
            w.move_to(Pos{terminal_origin + 0 * ch, row});
            w.clear_line();
        }
    } else if (hud_height_ > 0 * ln) {
        w.clear_screen();
    }

    w.restore_cursor();
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
}

void TerminalCompositor::set_hud_height(
    height_t hud_height, height_t term_height)
{
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
    auto old_has_separator =
        has_separator_hud(old_hud_height, old_term_height);
    auto new_has_separator =
        has_separator_hud(new_hud_height, term_height);

    auto old_scroll_bottom =
        old_has_separator ? row_index(scroll_bottom_for(
                                old_hud_height, old_term_height))
                          : -1;
    auto new_scroll_bottom =
        new_has_separator
            ? row_index(scroll_bottom_for(new_hud_height, term_height))
            : -1;

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

        // Shrink the scroll region only after pushing visible log lines up so
        // they remain on screen when the HUD claims rows.
        if (old_has_separator && new_has_separator
            && new_scroll_bottom < old_scroll_bottom) {
            auto scroll_diff = old_scroll_bottom - new_scroll_bottom;
            wr.move_to(
                Pos::at(0 * ch, lines(old_scroll_bottom)));
            wr.scroll_up(lines(scroll_diff));
        }

        if (new_has_separator) {
            hud_start_row_ =
                hud_start_row_for(new_hud_height, term_height);
            auto scroll_top = terminal_origin_v + 0 * ln;
            auto scroll_bottom =
                scroll_bottom_for(new_hud_height, term_height);
            wr.set_scroll_region(scroll_top, scroll_bottom);
        } else {
            // Full-screen or no-HUD mode.
            hud_start_row_ =
                hud_start_row_for(new_hud_height, term_height);
            wr.reset_scroll_region();
        }

        if (old_has_separator && new_has_separator
            && new_scroll_bottom > old_scroll_bottom) {
            auto scroll_diff = new_scroll_bottom - old_scroll_bottom;
            wr.move_to(Pos::origin());
            wr.scroll_down(lines(scroll_diff));
        }

        // Diff rendering will not write cells that are blank in both buffers,
        // so explicitly clear rows entering or leaving HUD ownership.
        int clear_start = std::min(
            chrome_start_row_for(old_hud_height, old_term_height),
            chrome_start_row_for(new_hud_height, term_height));
        if (old_has_separator && new_has_separator
            && new_scroll_bottom != old_scroll_bottom)
            clear_start =
                row_index(separator_row_for(new_hud_height, term_height));
        if (old_term_height == 0 * ln)
            clear_start = chrome_start_row_for(new_hud_height, term_height);
        clear_start = std::clamp(clear_start, 0, row_count(term_height));

        for (int row = clear_start; row < row_count(term_height); ++row) {
            wr.move_to(Pos::at(0 * ch, static_cast<std::size_t>(row) * ln));
            wr.clear_line();
        }

        wr.restore_cursor();
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.flush();
    }

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
    present_frame(std::cout);
}

void TerminalCompositor::present_frame(std::ostream & out)
{
    std::string buf;
    ansi::Writer w(buf);

    // Save cursor so HUD rendering doesn't disturb log output position
    w.save_cursor();

    if (has_separator_hud(hud_height_, term_height_)) {
        w.move_to(Pos{
            terminal_origin + 0 * ch,
            separator_row_for(hud_height_, term_height_)});
        w.reset();
//        for (std::size_t col = 0;
//             col < back_.width().count();
//             ++col)
//            w.text(separator_glyph);
        w.reset();
    }

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
                w.text(*text);
    });

    // Restore cursor to where it was before HUD render
    w.restore_cursor();

    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    out.flush();

    std::swap(front_, back_);

    back_ = front_;
}

} // namespace nxt::ui
