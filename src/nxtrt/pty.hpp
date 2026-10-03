#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/subprocess.hpp"
#include "nxtrt/task.hpp"

#include <nxtui/tui_terminal.hpp>
#include <nxtui/units.hpp>
#include <nxtui/vterm.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <utility>
#include <vector>

/**
 * @namespace nxtrt::pty
 * Programs on a pseudo-terminal, emulated into a screen nxtui can draw.
 *
 * `spawn` starts a program as session leader on a new PTY and returns a
 * `session`: the child, the PTY master, and an `nxtui::vterm::Terminal`
 * fed from the master. Run `session::read_loop` as a task to pump output
 * into the terminal until the child exits, write keystrokes with
 * `session::write_all`, and draw it with `pty_screen`.
 */
namespace nxtrt::pty {

using namespace std::chrono_literals;

/// What to run and the initial terminal size (cells).
struct spawn_options
{
    std::vector<std::string> argv;
    nxtui::Size size{80 * nxtui::ch, 24 * nxtui::ln};
};

inline winsize winsize_from(nxtui::Size size)
{
    return winsize{
        .ws_row = static_cast<unsigned short>(std::max<std::size_t>(
            1,
            size.h.count())),
        .ws_col = static_cast<unsigned short>(std::max<std::size_t>(
            1,
            size.w.count())),
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
}

/// A child on a PTY plus a terminal emulator of its screen.
///
/// Owns the child handle, the PTY master and the emulator; move-only. The
/// session must outlive any `read_loop`, `write_all` or
/// `terminate_and_wait` task on it and any `screen` that points at it.
/// Deck-confined.
class session
{
public:
    session() = default;

    explicit session(nxtrt::pty_child child, nxtui::Size size)
        : child_(std::move(child))
        , size_(size)
        , terminal_(
              static_cast<int>(std::max<std::size_t>(1, size.h.count())),
              static_cast<int>(std::max<std::size_t>(1, size.w.count())))
    {}

    session(const session &) = delete;
    session & operator=(const session &) = delete;
    session(session &&) noexcept = default;
    session & operator=(session &&) noexcept = default;

    [[nodiscard]] pid_t child_pid() const noexcept
    {
        return child_.pid;
    }

    [[nodiscard]] int master_fd() const noexcept
    {
        return child_.master_fd();
    }

    [[nodiscard]] nxtui::vterm::Terminal & terminal() noexcept
    {
        return terminal_;
    }

    [[nodiscard]] const nxtui::vterm::Terminal & terminal() const noexcept
    {
        return terminal_;
    }

    /// Resizes the PTY (TIOCSWINSZ, which signals SIGWINCH to the child)
    /// and the emulator. Zero sizes and unchanged sizes are ignored, and so
    /// is a PTY already gone (EBADF, EIO, ENOTTY); other ioctl failures
    /// throw `runtime_error`.
    void resize(nxtui::Size size)
    {
        if (size.w == 0 * nxtui::ch || size.h == 0 * nxtui::ln)
            return;
        if (size.w == size_.w && size.h == size_.h)
            return;

        size_ = size;
        auto ws = winsize_from(size);
        if (::ioctl(master_fd(), TIOCSWINSZ, &ws) < 0
            && errno != EBADF && errno != EIO && errno != ENOTTY)
            throw runtime_error{"ioctl(TIOCSWINSZ) failed"};
        terminal_.set_size(
            static_cast<int>(size.h.count()),
            static_cast<int>(size.w.count()));
    }

    /// Writes BYTES to the master, as typed input to the child.
    [[nodiscard]] task<void> write_all(std::string bytes)
    {
        auto offset = std::size_t{};
        while (offset < bytes.size()) {
            auto chunk = std::string_view{bytes}.substr(offset);
            auto written = co_await op::write_some{master_fd(), as_bytes(chunk)};
            if (written == 0)
                throw runtime_error{"pty write made no progress"};
            offset += written;
        }
    }

    /// Feeds the child's output into the terminal until it ends, then
    /// closes the master and returns the child's exit status.
    ///
    /// Replies the emulator generates (such as cursor position reports)
    /// are written back to the child. The loop ends at EOF or at any
    /// `runtime_error` from reading or replying, which includes the EIO a
    /// Linux master returns once the child side closes and also
    /// `operation_cancelled`; the final wait for the child is then subject
    /// to the same stop request.
    [[nodiscard]] task<child_result> read_loop()
    {
        auto storage = std::array<std::byte, 8192>{};

        while (true) {
            try {
                auto read = co_await op::read_some{
                    master_fd(),
                    std::span{storage}};
                if (read == 0)
                    break;

                terminal_.write(as_string_view(std::span{storage}.first(read)));
                auto reply = terminal_.read_pending_output();
                if (!reply.empty())
                    co_await write_all(std::move(reply));
            } catch (const runtime_error &) {
                break;
            }
        }

        child_.master.reset();
        co_return co_await subprocess::wait_child(child_);
    }

    /// Closes the master, then `subprocess::terminate_and_wait` (shielded
    /// SIGTERM, GRACE, SIGKILL). Do not run it after `read_loop` has
    /// returned on Linux, where that already reaped the child.
    [[nodiscard]] task<child_result> terminate_and_wait(
        std::chrono::milliseconds grace = 500ms)
    {
        child_.master.reset();
        co_return co_await subprocess::terminate_and_wait(child_, grace);
    }

private:
    nxtrt::pty_child child_;
    nxtui::Size size_{80 * nxtui::ch, 24 * nxtui::ln};
    nxtui::vterm::Terminal terminal_{24, 80};
};

/// Starts OPTIONS.argv (PATH-searched) as a session leader whose
/// controlling terminal is a new PTY of OPTIONS.size.
///
/// Exit status 127 means ARGV[0] was not found and 126 that the child
/// could not be set up; other failures throw `errno_error`. See
/// `nxtrt::spawn::pty`.
inline task<session> spawn(spawn_options options)
{
    auto columns = options.size.w.count();
    auto rows = options.size.h.count();
    auto child = co_await op::spawn_pty{
        std::move(options.argv),
        columns,
        rows};
    co_return session{std::move(child), options.size};
}

/// An nxtui element that draws a session's terminal, growing to fill its
/// space and resizing the PTY to the size it is drawn at. Borrows the
/// session.
struct screen
{
    session * pty = nullptr;
    nxtui::tui::Style clear_style{};

    constexpr nxtui::tui::WidthHint width_hint() const
    {
        return nxtui::tui::WidthHint::grow();
    }

    constexpr nxtui::tui::HeightHint height_hint() const
    {
        return nxtui::tui::HeightHint::grow();
    }

    void render(nxtui::RasterView & raster, nxtui::Size size) const
    {
        if (pty == nullptr)
            return;
        pty->resize(size);
        nxtui::tui::render_vterm_screen(
            raster,
            size,
            pty->terminal(),
            clear_style);
    }
};

/// A `screen` element for PTY.
inline screen pty_screen(session & pty, nxtui::tui::Style clear_style = {})
{
    return screen{.pty = &pty, .clear_style = clear_style};
}

} // namespace nxtrt::pty
