#pragma once

#include "nxtui/raster.hpp"

#include <optional>
#include <ranges>
#include <span>

namespace nxtui {

/// A run of adjacent changed cells on one row that share one style, before
/// style deltas are computed. `glyphs` points into the new raster.
struct RawChange
{
    /// Origin of the changed run.
    Pos origin;
    /// Glyph ids for the changed run.
    std::span<const GlyphTable::GlyphId> glyphs;
    /// New foreground color for the run.
    Rgba8 fg;
    /// New background color for the run.
    Rgba8 bg;
    /// New emphasis for the run.
    Emphasis em;
};

/// A run of changed cells, with color/emphasis deltas for minimal ANSI
/// output.
struct ChangeRun
{
    /// Origin of the emitted run.
    Pos origin;
    /// Glyph ids for the emitted run.
    std::span<const GlyphTable::GlyphId> glyphs;
    /// Foreground change to emit before text, if any.
    std::optional<Rgba8> fg_change;
    /// Background change to emit before text, if any.
    std::optional<Rgba8> bg_change;
    /// Emphasis change to emit before text, if any.
    std::optional<Emphasis> em_change;
    /// Whether foreground should be reset before text.
    bool fg_reset = false;
    /// Whether background should be reset before text.
    bool bg_reset = false;
    /// Whether emphasis should be reset before text.
    bool em_reset = false;
};

/// Tracks the SGR state already emitted in this frame and turns each
/// `RawChange` into a `ChangeRun` carrying only the needed changes.
///
/// State starts unknown (treated as terminal defaults). A run whose color
/// is `terminal_default()` gets a reset flag. An emphasis change between two
/// non-empty sets is reported as `em_change` with the new set and no reset,
/// so a writer that only adds attributes leaves the old bits on.
struct StyleState
{
    /// Current emitted foreground state.
    std::optional<Rgba8> fg;
    /// Current emitted background state.
    std::optional<Rgba8> bg;
    /// Current emitted emphasis state.
    std::optional<Emphasis> em;

    /// Convert a raw run into a run annotated with style deltas.
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

        // SGR emphasis parameters only turn attributes on. If the next
        // style drops any currently active attribute, reset first;
        // otherwise emit the changed nonempty style without disturbing
        // existing attributes.
        if (em && ((*em & raw.em) != *em))
            run.em_reset = true;

        if (raw.em != Emphasis::none && raw.em != em)
            run.em_change = raw.em;
        em =
            raw.em == Emphasis::none ? std::nullopt : std::optional{raw.em};

        return run;
    }
};

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

/// Call `emit(const ChangeRun &)` for each changed run between `front`
/// (what the terminal shows) and `back` (the new frame), row by row, left
/// to right. Both rasters must have the same size. Runs split wherever the
/// change status or the new cell style changes.
template<typename F>
void diff_rasters(const Raster & front, const Raster & back, F && emit)
{
    StyleState style;
    for (const auto & raw : raw_changes(front, back))
        emit(style(raw));
}

} // namespace nxtui
