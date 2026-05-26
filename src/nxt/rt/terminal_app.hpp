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

namespace nxt::rt {

struct terminal_app_options
{
    bool raw_input = true;
    bool alternate_screen = false;
    bool hide_cursor = true;
    bool clear_screen = true;
    nxtui::Size fallback_size{96 * nxtui::ch, 26 * nxtui::ln};
};

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

} // namespace nxt::rt
