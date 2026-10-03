#pragma once

#include <nxtui/ansi.hpp>
#include <nxtui/compositor.hpp>
#include <nxtui/glyph-table.hpp>
#include <nxtui/units.hpp>

#include <fcntl.h>
#include <iostream>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace nxtrt {

/// Terminal modes `terminal_app` sets up for its lifetime.
struct terminal_app_options
{
    bool raw_input = true;
    bool alternate_screen = false;
    bool hide_cursor = true;
    bool clear_screen = true;
    nxtui::Size fallback_size{96 * nxtui::ch, 26 * nxtui::ln};
};

/// The size of the terminal on stdout in cells, or FALLBACK when stdout is
/// not a terminal or reports zero.
inline nxtui::Size current_terminal_size(
    nxtui::Size fallback = {96 * nxtui::ch, 26 * nxtui::ln})
{
    auto ws = winsize{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
        && ws.ws_col > 0
        && ws.ws_row > 0) {
        return nxtui::Size{
            static_cast<std::size_t>(ws.ws_col) * nxtui::ch,
            static_cast<std::size_t>(ws.ws_row) * nxtui::ln,
        };
    }
    return fallback;
}

/// RAII raw mode for a terminal input descriptor.
///
/// When ENABLED and FD is a terminal, turns off echo, canonical input,
/// signal keys (so Ctrl-C arrives as a byte) and flow control, and makes
/// reads return immediately (VMIN = VTIME = 0). It also sets O_NONBLOCK on
/// FD (whenever ENABLED and FD is a terminal). The destructor restores the
/// saved attributes and file status flags.
class raw_terminal_mode
{
public:
    explicit raw_terminal_mode(int fd = STDIN_FILENO, bool enabled = true)
        : fd_(fd)
        , active_(enabled && ::isatty(fd) && ::tcgetattr(fd, &saved_) == 0)
    {
        if (!active_)
            return;

        auto raw = saved_;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(fd_, TCSANOW, &raw) != 0)
            active_ = false;

        saved_flags_ = ::fcntl(fd_, F_GETFL, 0);
        if (saved_flags_ >= 0)
            flags_active_ =
                ::fcntl(fd_, F_SETFL, saved_flags_ | O_NONBLOCK) == 0;
    }

    raw_terminal_mode(const raw_terminal_mode &) = delete;
    raw_terminal_mode & operator=(const raw_terminal_mode &) = delete;

    ~raw_terminal_mode()
    {
        if (flags_active_)
            (void)::fcntl(fd_, F_SETFL, saved_flags_);
        if (active_)
            (void)::tcsetattr(fd_, TCSANOW, &saved_);
    }

private:
    int fd_ = -1;
    int saved_flags_ = -1;
    termios saved_{};
    bool active_ = false;
    bool flags_active_ = false;
};

/// RAII setup of stdin/stdout for a full-screen nxtui program.
///
/// Construction puts stdin in raw mode (if `raw_input`), enables ANSI
/// output, optionally switches to the alternate screen, hides the cursor
/// and clears the screen, and creates a `TerminalCompositor` at the current
/// size. Destruction shows the cursor again if it was hidden, resets
/// attributes, leaves the alternate screen if it entered it, and restores
/// stdin. Writes go to `std::cout`. Use one at a time; not copyable.
///
/// The size is not tracked automatically: call `refresh_size` (for example
/// on SIGWINCH or before each frame) to pick up a new terminal size.
class terminal_app
{
public:
    explicit terminal_app(terminal_app_options options = {})
        : options_(options)
        , raw_(STDIN_FILENO, options.raw_input)
        , size_(current_terminal_size(options.fallback_size))
        , compositor_(size_, glyphs_)
    {
        nxtui::ansi::init();
        nxtui::ansi::mode = nxtui::ansi::Mode::enabled;

        if (options_.alternate_screen)
            std::cout << "\x1b[?1049h";
        if (options_.hide_cursor)
            std::cout << "\x1b[?25l";
        if (options_.clear_screen)
            std::cout << "\x1b[2J\x1b[H";
        std::cout << std::flush;
    }

    terminal_app(const terminal_app &) = delete;
    terminal_app & operator=(const terminal_app &) = delete;

    ~terminal_app()
    {
        if (options_.hide_cursor)
            std::cout << "\x1b[?25h";
        std::cout << "\x1b[0m";
        if (options_.alternate_screen)
            std::cout << "\x1b[?1049l";
        std::cout << std::flush;
    }

    [[nodiscard]] nxtui::Size size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] nxtui::tui::TerminalCompositor & compositor() noexcept
    {
        return compositor_;
    }

    /// Re-reads the terminal size and resizes the compositor; true if it
    /// changed.
    [[nodiscard]] bool refresh_size()
    {
        auto next = current_terminal_size(options_.fallback_size);
        if (next.w == size_.w && next.h == size_.h)
            return false;
        size_ = next;
        compositor_.resize(size_);
        return true;
    }

private:
    terminal_app_options options_;
    raw_terminal_mode raw_;
    nxtui::GlyphTable glyphs_;
    nxtui::Size size_;
    nxtui::tui::TerminalCompositor compositor_;
};

} // namespace nxtrt
