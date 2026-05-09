#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "nxt/raster.hpp"
#include "nxt/units.hpp"

namespace nxt::ansi {

/// ANSI output modes
enum class Mode {
    disabled,  // No ANSI output at all (default for non-TTY)
    debug,     // Readable debug format like ⟨CSI:0m⟩
    enabled    // Real ANSI escape sequences (default for TTY)
};

/// Current ANSI output mode
extern Mode mode;

/// Initialize the ANSI module. Sets mode based on TTY detection.
/// Call this early in main().
void init();

/// Check if stdout is connected to a real TTY
[[nodiscard]] bool is_tty();

enum class TerminalColor : int {
    black = 30,
    red = 31,
    green = 32,
    yellow = 33,
    blue = 34,
    magenta = 35,
    cyan = 36,
    white = 37,
    bright_black = 90,
    bright_red = 91,
    bright_green = 92,
    bright_yellow = 93,
    bright_blue = 94,
    bright_magenta = 95,
    bright_cyan = 96,
    bright_white = 97,
};

/// ANSI escape sequence builder that writes to a string buffer.
class Writer
{
public:
    explicit Writer(std::string & buf)
        : buf_(buf)
    {
    }

    /// Move cursor to terminal position (1-based coordinates)
    Writer & move_to(ansi_row_t y, ansi_col_t x);
    Writer & move_to(Pos pos);

    /// Move cursor relatively (positive deltas)
    Writer & move_up(height_t n = 1 * ln);
    Writer & move_down(height_t n = 1 * ln);
    Writer & move_right(width_t n = 1 * ch);
    Writer & move_left(width_t n = 1 * ch);
    Writer & move(Size delta); // right and down only

    /// Move to column (1-based)
    Writer & move_to_column(ansi_col_t col);

    /// Clear operations
    Writer & clear_screen();
    Writer & clear_screen_from_cursor();
    Writer & clear_screen_to_cursor();
    Writer & clear_line();
    Writer & clear_line_from_cursor();
    Writer & clear_line_to_cursor();

    /// Scroll region (INCLUSIVE bounds [top, bottom])
    Writer & set_scroll_region(row_t top, row_t bottom);
    Writer & reset_scroll_region();
    Writer & scroll_up(height_t n = 1 * ln);
    Writer & scroll_down(height_t n = 1 * ln);

    /// Cursor visibility
    Writer & hide_cursor();
    Writer & show_cursor();

    /// Terminal synchronized update mode. Supporting terminals defer
    /// presenting changes until the matching end call.
    Writer & begin_synchronized_update();
    Writer & end_synchronized_update();

    /// Save/restore cursor position
    Writer & save_cursor();
    Writer & restore_cursor();

    /// Request cursor position report (DSR 6). Response comes via stdin.
    Writer & request_cursor_position();

    /// Colors (24-bit RGB)
    Writer & fg(Rgb8 color);
    Writer & fg(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    Writer & bg(Rgb8 color);
    Writer & bg(std::uint8_t r, std::uint8_t g, std::uint8_t b);

    /// Terminal colors (16-color palette)
    Writer & fg(TerminalColor c);
    Writer & bg(TerminalColor c);

    /// 256-color palette
    Writer & fg_palette(std::uint8_t index);
    Writer & bg_palette(std::uint8_t index);

    /// Reset colors
    Writer & fg_default();
    Writer & bg_default();

    /// Text emphasis.
    Writer & style(Emphasis e);
    Writer & reset();
    Writer & bold();
    Writer & dim();
    Writer & italic();
    Writer & underline();
    Writer & reverse();

    /// Write raw text (no escaping)
    Writer & text(std::string_view str);

    /// Write a single character
    Writer & text(const char ch)
    {
        buf_.push_back(ch);
        return *this;
    }

    [[nodiscard]] std::string & buffer()
    {
        return buf_;
    }

    [[nodiscard]] const std::string & buffer() const
    {
        return buf_;
    }

private:
    std::string & buf_;

    /// Helper: write CSI sequence
    void csi(std::string_view params, char final_byte);
};

/// Standalone functions for immediate output (writes directly to
/// stdout)

void move_to(ansi_row_t row, ansi_col_t col);
void move_to(Pos pos);
void clear_screen();
void clear_line();
void hide_cursor();
void show_cursor();
void begin_synchronized_update();
void end_synchronized_update();
void set_scroll_region(row_t top, row_t bottom);
void reset_scroll_region();
void scroll_up(height_t n = 1 * ln);
void scroll_down(height_t n = 1 * ln);

/// Query current cursor position (blocking).
/// Sends DSR 6 and reads CPR response from stdin.
/// Returns nullopt if query fails (not a TTY, timeout, parse error).
/// Requires terminal to be in raw mode for reliable response reading.
[[nodiscard]] std::optional<Pos> query_cursor_position();

/// RAII guard that hides cursor on construction, restores terminal
/// state on destruction. Resets scroll region, shows cursor, clears
/// screen.
struct TerminalGuard
{
    TerminalGuard();
    ~TerminalGuard();

    TerminalGuard(const TerminalGuard &) = delete;
    TerminalGuard & operator=(const TerminalGuard &) = delete;
};

struct SynchronizedUpdate
{
    explicit SynchronizedUpdate(bool enabled = true);
    ~SynchronizedUpdate();

    SynchronizedUpdate(const SynchronizedUpdate &) = delete;
    SynchronizedUpdate & operator=(const SynchronizedUpdate &) = delete;

private:
    bool enabled_{false};
};

} // namespace nxt::ansi
