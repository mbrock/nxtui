#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "nxtui/raster.hpp"
#include "nxtui/units.hpp"

/**
 * @namespace nxtui::ansi
 * ANSI escape-sequence output: `Writer` appends sequences to a string, the
 * free functions write them straight to `std::cout`, and `mode` decides
 * whether sequences are real, shown in a readable debug form, or omitted.
 *
 * Coordinates are zero-based `Pos`/`row_t`/`col_t` values; the writer
 * converts to the one-based numbers in the sequences. Call `init()` early
 * in `main` so `mode` matches the output device.
 */
namespace nxtui::ansi {

/// ANSI output modes
enum class Mode {
    disabled,  // No ANSI output at all
    debug,     // Readable debug format like ⟨CSI:0m⟩ (default for non-TTY)
    enabled    // Real ANSI escape sequences (default for TTY)
};

/// Current ANSI output mode, shared process-wide and read on every escape
/// written. `disabled` until `init()` runs or it is assigned.
extern Mode mode;

/// Initialize the ANSI module. Sets mode based on TTY detection,
/// then applies the NXT_ANSI env var override if set
/// (values: disabled|off|0, debug, enabled|on|1). Call early in main().
void init();

/// Check if stdout is connected to a real TTY
[[nodiscard]] bool is_tty();

/// ANSI 16-color SGR foreground codes.
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

/// Appends escape sequences, formatted per `mode`, to a borrowed string.
///
/// Every method returns `*this` for chaining. The buffer must outlive the
/// writer. Text written with `text` is appended raw, without escaping and
/// regardless of `mode`.
class Writer
{
public:
    explicit Writer(std::string & buf)
        : buf_(buf)
    {
    }

    /// Move the cursor to a zero-based position (CUP). Note the
    /// row-then-column argument order of the first overload.
    Writer & move_to(ansi_row_t y, ansi_col_t x);
    Writer & move_to(Pos pos);

    /// Move the cursor relatively; a zero count writes nothing.
    Writer & move_up(height_t n = 1 * ln);
    Writer & move_down(height_t n = 1 * ln);
    Writer & move_right(width_t n = 1 * ch);
    Writer & move_left(width_t n = 1 * ch);
    Writer & move(Size delta); // right and down only

    /// Move to a zero-based column on the current row (CHA).
    Writer & move_to_column(ansi_col_t col);

    /// Clear operations
    Writer & clear_screen();
    Writer & clear_screen_from_cursor();
    Writer & clear_screen_to_cursor();
    Writer & clear_line();
    Writer & clear_line_from_cursor();
    Writer & clear_line_to_cursor();

    /// Set the scroll region (DECSTBM) to the inclusive zero-based rows
    /// [`top`, `bottom`]. Terminals home the cursor when this runs.
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

    /// Request a cursor position report (DSR 6). The reply arrives on stdin
    /// as `CSI row ; col R`; `input::Parser` decodes it into
    /// `KeyEvent::cursor_position`.
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

    /// Turn on each attribute in `e`. Attributes already on stay on; use
    /// `reset()` (SGR 0, which also resets colors) to clear them.
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

/// Render a raster as inline SGR-styled text suitable for scrollback
/// output.
[[nodiscard]] std::string render_raster(const Raster & raster);

/// Write a cursor move straight to `std::cout`. The free functions below
/// mirror the `Writer` methods of the same names.
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

/// Ask the terminal for the cursor position and wait for the reply.
///
/// Sends DSR 6 to stdout and reads the reply from stdin synchronously,
/// temporarily turning off canonical mode and echo, giving up after about
/// 100 ms without input. Returns a zero-based position, or `std::nullopt`
/// if stdout is not a TTY, nothing arrives, or the reply does not parse.
/// Other input arriving meanwhile is consumed and lost.
[[nodiscard]] std::optional<Pos> query_cursor_position();

/// RAII guard for a TUI session: the constructor calls `init()` and hides
/// the cursor; the destructor resets the scroll region (keeping the cursor
/// position), resets SGR attributes, and shows the cursor.
struct TerminalGuard
{
    /// Enter application-friendly terminal state.
    TerminalGuard();
    /// Restore cursor, scroll region, and screen state.
    ~TerminalGuard();

    TerminalGuard(const TerminalGuard &) = delete;
    TerminalGuard & operator=(const TerminalGuard &) = delete;
};

/// RAII wrapper for terminal synchronized update mode.
struct SynchronizedUpdate
{
    /// Begin synchronized update mode when enabled.
    explicit SynchronizedUpdate(bool enabled = true);
    /// End synchronized update mode when it was enabled.
    ~SynchronizedUpdate();

    SynchronizedUpdate(const SynchronizedUpdate &) = delete;
    SynchronizedUpdate & operator=(const SynchronizedUpdate &) = delete;

private:
    bool enabled_{false};
};

} // namespace nxtui::ansi
