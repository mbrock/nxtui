// Single-line text input widget.
//
// Two pieces:
//   - TextField:   editable state (text + cursor byte-offset) and
//                  the small set of edit operations a single-line
//                  input needs (insert, erase, move, clear).
//   - text_field:  a flexing leaf that draws a TextField into a
//                  RasterView, handling scroll-to-cursor, prefix,
//                  placeholder, focus state, and the block cursor.
//
// The two are decoupled: the leaf takes a snapshot of the field's
// text and cursor, so it stays valid even if the original TextField
// changes between construction and render.

#pragma once

#include "nxt/raster.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"
#include "nxt/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace nxt::tui {

/// Single-line editable text with a UTF-8-aware cursor.
///
/// `cursor_byte` is always at a UTF-8 boundary in the range [0, text.size()].
/// Edit methods clamp it defensively in case `text` was modified directly.
struct TextField
{
    std::string text;
    utf8::byte_offset_t cursor_byte{};

    /// Insert UTF-8 bytes at the cursor and advance.
    void insert(std::string_view bytes)
    {
        cursor_byte = utf8::floor_boundary(text, cursor_byte);
        text.insert(cursor_byte.count(), bytes);
        cursor_byte += bytes.size();
    }

    /// Delete the UTF-8 cell to the left of the cursor (backspace).
    /// Returns true if anything was deleted.
    bool erase_left() noexcept
    {
        cursor_byte = utf8::floor_boundary(text, cursor_byte);
        if (cursor_byte.count() == 0)
            return false;
        auto p = utf8::prev(text, cursor_byte);
        text.erase(p.count(), cursor_byte - p);
        cursor_byte = p;
        return true;
    }

    /// Delete the UTF-8 cell to the right of the cursor (forward delete).
    bool erase_right() noexcept
    {
        cursor_byte = utf8::floor_boundary(text, cursor_byte);
        if (cursor_byte.count() >= text.size())
            return false;
        auto n = utf8::next(text, cursor_byte);
        text.erase(cursor_byte.count(), n - cursor_byte);
        return true;
    }

    bool move_left() noexcept
    {
        cursor_byte = utf8::floor_boundary(text, cursor_byte);
        if (cursor_byte.count() == 0)
            return false;
        cursor_byte = utf8::prev(text, cursor_byte);
        return true;
    }

    bool move_right() noexcept
    {
        cursor_byte = utf8::floor_boundary(text, cursor_byte);
        if (cursor_byte.count() >= text.size())
            return false;
        cursor_byte = utf8::next(text, cursor_byte);
        return true;
    }

    bool move_home() noexcept
    {
        if (cursor_byte.count() == 0)
            return false;
        cursor_byte = utf8::byte_offset(0);
        return true;
    }

    bool move_end() noexcept
    {
        if (cursor_byte.count() >= text.size())
            return false;
        cursor_byte = utf8::byte_offset(text.size());
        return true;
    }

    void clear() noexcept
    {
        text.clear();
        cursor_byte = utf8::byte_offset(0);
    }

    [[nodiscard]] bool empty() const noexcept { return text.empty(); }

    /// Display column count.
    [[nodiscard]] width_t cell_count() const noexcept
    {
        return utf8::display_width(text);
    }

    /// Cursor column.
    [[nodiscard]] width_t cursor_cell() const noexcept
    {
        return utf8::column_at(text, cursor_byte);
    }
};

struct TextFieldStyle
{
    Rgba8 fg = Rgba8::terminal_default();
    Rgba8 bg = Rgba8::terminal_default();
    Rgba8 prefix_fg = Rgba8::terminal_default();
    Rgba8 placeholder_fg = Rgba8::terminal_default();
};

struct TextFieldOptions
{
    /// Static prefix (e.g. "> ").  Always visible at column 0.
    std::string_view prefix{};

    /// Shown in `placeholder_fg` when the field is empty.
    std::string_view placeholder{};

    TextFieldStyle style{};

    /// When false, the cursor is not drawn and editing keys (if you
    /// pass them in) should be ignored by the caller.
    bool focused{true};
};

namespace detail {

/// First visible display column that keeps `cursor_cell` on screen, given
/// a viewport of `field_w` cells.  Cursor stays within the window;
/// when it reaches the right edge we scroll one cell at a time.
[[nodiscard]] inline width_t scroll_for_cursor(
    width_t cursor_cell, width_t field_w) noexcept
{
    if (field_w == 0 * ch || cursor_cell < field_w)
        return 0 * ch;
    return cursor_cell - field_w + 1 * ch;
}

struct TextFieldRender
{
    std::string text;
    utf8::byte_offset_t cursor_byte;
    std::string prefix;
    std::string placeholder;
    TextFieldStyle style;
    bool focused;
};

}  // namespace detail

/// A flexing single-line text input leaf.
///
/// Width: grows.  Height: 1 line.  The TextField is snapshot by value
/// so the leaf is safe to render later.
inline auto text_field(
    const TextField & field, TextFieldOptions opts = {})
{
    detail::TextFieldRender r{
        .text = field.text,
        .cursor_byte = field.cursor_byte,
        .prefix = std::string{opts.prefix},
        .placeholder = std::string{opts.placeholder},
        .style = opts.style,
        .focused = opts.focused,
    };

    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * ln),
        [r = std::move(r)](RasterView & view, Size size) {
            const auto total_w = size.w.count();
            if (total_w == 0)
                return;

            const auto prefix_w =
                std::min(utf8::display_width(r.prefix), size.w);
            const auto field_w = size.w - prefix_w;
            const auto prefix_cells = prefix_w.count();

            // Fill the whole row first so scrolled-off text and trailing
            // blanks still carry the field style.
            for (std::size_t x = 0; x < total_w; ++x) {
                auto pos = Pos::at(x * ch, 0 * ln);
                view.set_bg(pos, r.style.bg);
                view.set_fg(pos, r.style.fg);
            }

            if (prefix_cells > 0) {
                for (std::size_t x = 0; x < prefix_cells; ++x)
                    view.set_fg(
                        Pos::at(x * ch, 0 * ln), r.style.prefix_fg);
                view.write_text(Pos::origin(), r.prefix);
            }

            const auto cursor_cell = utf8::column_at(r.text, r.cursor_byte);
            const auto scroll_cell =
                detail::scroll_for_cursor(cursor_cell, field_w);

            if (r.text.empty() && !r.placeholder.empty()
                && field_w > 0 * ch) {
                for (std::size_t x = prefix_cells; x < total_w; ++x)
                    view.set_fg(
                        Pos::at(x * ch, 0 * ln), r.style.placeholder_fg);
                view.write_text(
                    Pos::at(prefix_cells * ch, 0 * ln), r.placeholder);
            } else if (field_w > 0 * ch) {
                const auto start_byte =
                    utf8::byte_at_column(r.text, scroll_cell);
                const auto end_byte =
                    utf8::byte_at_column(r.text, scroll_cell + field_w);
                view.write_text(
                    Pos::at(prefix_cells * ch, 0 * ln),
                    std::string_view{r.text}.substr(
                        start_byte.count(), end_byte - start_byte));
            }

            // The cursor may sit one column past the last glyph; the
            // pre-filled row gives that blank cell something to reverse.
            if (r.focused && field_w > 0 * ch) {
                const auto visible_cursor =
                    (cursor_cell - scroll_cell).count();
                const auto cursor_x = std::min(
                    prefix_cells + visible_cursor, total_w - 1);
                view.set_em(
                    Pos::at(cursor_x * ch, 0 * ln), Emphasis::reverse);
            }
        });
}

}  // namespace nxt::tui
