#pragma once

#include "nxtrt/task.hpp"
#include "nxt/unique-fd.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

namespace nxtrt {

/// Manual-reset deck-local notification backed by platform fd readiness.
///
/// `ring()` makes the bell readable and wakes tasks waiting through the active
/// wand's ordinary poll machinery. `reset()` silences the bell and drains the
/// readiness token so future waits block again.
class bell
{
public:
    bell()
    {
        open();
    }

    bell(const bell &) = delete;
    bell & operator=(const bell &) = delete;
    bell(bell &&) = delete;
    bell & operator=(bell &&) = delete;

    ~bell()
    {
        ring();
    }

    [[nodiscard]] bool ringing() const noexcept
    {
        return ringing_;
    }

    [[nodiscard]] bool is_set() const noexcept
    {
        return ringing();
    }

    void ring()
    {
        if (ringing_)
            return;
        ringing_ = true;
        notify();
    }

    void set()
    {
        ring();
    }

    void reset()
    {
        ringing_ = false;
        drain();
    }

    [[nodiscard]] hope<void> wait()
    {
        if (ringing_)
            return hope<void>::ready();
        return wait_slow();
    }

    [[nodiscard]] auto operator co_await()
    {
        return wait();
    }

private:
    void open()
    {
#if defined(__linux__)
        fd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (fd_.get() >= 0)
            return;
#endif

        auto fds = std::array<int, 2>{-1, -1};
        if (::pipe(fds.data()) != 0)
            throw runtime_error{"bell pipe failed"};
        read_fd_.reset(fds[0]);
        fd_.reset(fds[1]);
        set_nonblocking(read_fd_.get());
        set_nonblocking(fd_.get());
        set_close_on_exec(read_fd_.get());
        set_close_on_exec(fd_.get());
    }

    static void set_nonblocking(int fd)
    {
        auto flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    static void set_close_on_exec(int fd)
    {
        auto flags = ::fcntl(fd, F_GETFD, 0);
        if (flags >= 0)
            (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }

    [[nodiscard]] int poll_fd() const noexcept
    {
        return read_fd_.get() >= 0 ? read_fd_.get() : fd_.get();
    }

    void notify() noexcept
    {
#if defined(__linux__)
        if (read_fd_.get() < 0) {
            auto value = std::uint64_t{1};
            while (::write(fd_.get(), &value, sizeof(value)) < 0
                   && errno == EINTR) {
            }
            return;
        }
#endif

        auto byte = std::uint8_t{1};
        while (::write(fd_.get(), &byte, sizeof(byte)) < 0 && errno == EINTR) {
        }
    }

    void drain() noexcept
    {
#if defined(__linux__)
        if (read_fd_.get() < 0) {
            auto value = std::uint64_t{};
            while (true) {
                auto n = ::read(fd_.get(), &value, sizeof(value));
                if (n >= 0)
                    continue;
                if (errno == EINTR)
                    continue;
                break;
            }
            return;
        }
#endif

        auto bytes = std::array<std::byte, 64>{};
        while (true) {
            auto n = ::read(read_fd_.get(), bytes.data(), bytes.size());
            if (n > 0)
                continue;
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
    }

    task<void> wait_slow()
    {
        while (!ringing_) {
            auto events = co_await op::poll{poll_fd(), POLLIN};
            if ((events & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0)
                co_return;
        }
    }

    nxt::unique_fd fd_;
    nxt::unique_fd read_fd_;
    bool ringing_ = false;
};

} // namespace nxtrt
