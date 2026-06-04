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

namespace nxtrt::pty {

using namespace std::chrono_literals;

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

inline screen pty_screen(session & pty, nxtui::tui::Style clear_style = {})
{
    return screen{.pty = &pty, .clear_style = clear_style};
}

} // namespace nxtrt::pty
