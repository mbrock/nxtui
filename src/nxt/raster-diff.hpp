#pragma once

#include "nxt/raster.hpp"

#include <optional>
#include <ranges>
#include <span>

namespace nxt {

// ============================================================================
// Data types
// ============================================================================

/// A detected change: position, glyphs, colors, emphasis (before
/// optimization).
struct RawChange
{
    Pos origin;
    std::span<const GlyphTable::GlyphId> glyphs;
    Rgba8 fg;
    Rgba8 bg;
    Emphasis em;
};

/// A run of changed cells, with color/emphasis deltas for minimal ANSI
/// output.
struct ChangeRun
{
    Pos origin;
    std::span<const GlyphTable::GlyphId> glyphs;
    std::optional<Rgba8> fg_change;
    std::optional<Rgba8> bg_change;
    std::optional<Emphasis> em_change;
    bool fg_reset = false;
    bool bg_reset = false;
    bool em_reset = false;
};

/// Tracks terminal style state, converting RawChange → ChangeRun with
/// deltas.
struct StyleState
{
    std::optional<Rgba8> fg;
    std::optional<Rgba8> bg;
    std::optional<Emphasis> em;

    ChangeRun operator()(const RawChange & raw)
    {
        ChangeRun run{
            raw.origin, raw.glyphs, {}, {}, {}, false, false, false};

        if (raw.fg.is_terminal_default() && fg) {
            run.fg_reset = true;
            fg = std::nullopt;
        } else if (!raw.fg.is_terminal_default() && raw.fg != fg) {
            run.fg_change = raw.fg;
            fg = raw.fg;
        }

        if (raw.bg.is_terminal_default() && bg) {
            run.bg_reset = true;
            bg = std::nullopt;
        } else if (!raw.bg.is_terminal_default() && raw.bg != bg) {
            run.bg_change = raw.bg;
            bg = raw.bg;
        }

        if (raw.em == Emphasis::none && em) {
            run.em_reset = true;
            em = std::nullopt;
        } else if (raw.em != Emphasis::none && raw.em != em) {
            run.em_change = raw.em;
            em = raw.em;
        }

        return run;
    }
};

// ============================================================================
// Pipeline building blocks
// ============================================================================

/// Did this cell change? (comparing old vs new from a zipped pair)
constexpr auto is_changed = [](const auto & pair) {
    const auto & [old_cell, new_cell] = pair;
    return old_cell != new_cell;
};

/// Should two cell pairs belong in the same run?
/// Yes if: both changed (or both unchanged) AND same style in new
/// buffer.
constexpr auto same_run = [](const auto & a, const auto & b) {
    const auto & [old_a, new_a] = a;
    const auto & [old_b, new_b] = b;
    return is_changed(a) == is_changed(b) // same change status
           && new_a.fg == new_b.fg        // same foreground
           && new_a.bg == new_b.bg        // same background
           && new_a.em == new_b.em;       // same emphasis
};

// ============================================================================
// The diff pipeline
// ============================================================================

/// Extract changed runs from a single row.
/// Groups cells by run boundaries, keeps only changed runs, converts to
/// RawChange.
inline auto row_changes(height_t y, auto cells, const Raster & back)
{
    return cells | std::views::chunk_by(same_run)
           | std::views::filter(
               [](auto chunk) { return is_changed(*chunk.begin()); })
           | std::views::transform([y, &back](auto chunk) {
                 const auto & [old_cell, new_cell] = *chunk.begin();
                 return RawChange{
                     .origin = Pos::at(new_cell.col, y),
                     .glyphs = back.glyph_span(
                         y, new_cell.col, std::ranges::distance(chunk)),
                     .fg = new_cell.fg,
                     .bg = new_cell.bg,
                     .em = new_cell.em,
                 };
             });
}

/// Iterate changed regions between two rasters as a lazy range.
///
/// Pipeline:
///   zip_rows(front, back)    -- pair up rows from both rasters
///   | transform(row_changes) -- extract changed runs from each row
///   | join                   -- flatten into single stream
///
inline auto raw_changes(const Raster & front, const Raster & back)
{
    return zip_rows(front, back)
           | std::views::transform([&back](auto row_pair) {
                 auto [y, cells] = row_pair;
                 return row_changes(y, cells, back);
             })
           | std::views::join;
}

/// Iterate changed regions, tracking style state for minimal ANSI
/// output.
template<typename F>
void diff_rasters(const Raster & front, const Raster & back, F && emit)
{
    StyleState style;
    for (const auto & raw : raw_changes(front, back))
        emit(style(raw));
}

} // namespace nxt
