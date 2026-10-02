#pragma once

#include "wish.hpp"

#include "nxt/unique-fd.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <utility>
#include <variant>
#include <vector>

#if defined(__linux__)
#include <linux/openat2.h>
#include <linux/stat.h>
#include <linux/time_types.h>
#else
#include <cstdint>
#endif

namespace nxtrt {

#if defined(__linux__)
using kernel_timespec = __kernel_timespec;
#else
struct kernel_timespec
{
    std::int64_t tv_sec = 0;
    std::int64_t tv_nsec = 0;
};
#endif

inline kernel_timespec as_kernel_timespec(std::chrono::nanoseconds duration)
{
    if (duration < std::chrono::nanoseconds::zero())
        duration = std::chrono::nanoseconds::zero();

    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
    return kernel_timespec{
        .tv_sec = seconds.count(),
        .tv_nsec = nanoseconds.count(),
    };
}

/// Result of waiting for file-descriptor readiness until a deadline.
struct poll_until_result
{
    /// Poll revents when readiness wins; zero when the deadline wins.
    int events = 0;
    /// True when the deadline completed before fd readiness.
    bool timed_out = false;
};

#if defined(__linux__)
using statx_result = struct statx;
#endif

struct child_result
{
    pid_t pid = -1;
    int code = 0;
    bool exited = false;
    int exit_code = 0;
    bool signaled = false;
    int signal = 0;
};

#if defined(__linux__)
/// A pidfd: waits and signals through it can never reach a reused pid.
using child_handle = nxt::unique_fd;
#else
/// The child's pid. An unreaped pid cannot be reused, and wait_child reads
/// the exit status without reaping (WNOWAIT), so signals can never reach
/// another process while this handle lives. The handle reaps the child when
/// destroyed, if it has exited; one still running then stays a zombie
/// after it exits, as it would on Linux once its pidfd is closed.
class child_handle
{
public:
    explicit child_handle(pid_t pid = -1) noexcept
        : pid_(pid)
    {}

    child_handle(child_handle && other) noexcept
        : pid_(std::exchange(other.pid_, -1))
    {}

    child_handle & operator=(child_handle && other) noexcept
    {
        if (this != &other)
            reset(std::exchange(other.pid_, -1));
        return *this;
    }

    child_handle(const child_handle &) = delete;
    child_handle & operator=(const child_handle &) = delete;

    ~child_handle()
    {
        reset();
    }

    [[nodiscard]] int get() const noexcept
    {
        return pid_;
    }

    void reset(pid_t pid = -1) noexcept
    {
        if (pid_ > 0)
            (void) ::waitpid(pid_, nullptr, WNOHANG);
        pid_ = pid;
    }

private:
    pid_t pid_ = -1;
};
#endif

/// A child whose stdout and stderr share one pipe; stdin is /dev/null.
struct piped_child
{
    pid_t pid = -1;
    child_handle handle{};
    nxt::unique_fd output{};

    /// What wait_child and signal_child take: a pidfd on Linux, the pid
    /// elsewhere.
    [[nodiscard]] int child_ref() const noexcept
    {
        return handle.get();
    }

    [[nodiscard]] int output_fd() const noexcept
    {
        return output.get();
    }
};

/// A child running as session leader on a PTY whose master we hold.
struct pty_child
{
    pid_t pid = -1;
    child_handle handle{};
    nxt::unique_fd master{};

    [[nodiscard]] int child_ref() const noexcept
    {
        return handle.get();
    }

    [[nodiscard]] int master_fd() const noexcept
    {
        return master.get();
    }
};

namespace op {

/// Closed operation type for deterministic/manual tests.
///
/// This is deliberately more like a tiny SQE recipe than a generic variant:
/// the operation owns its input parameters and names its result type.
struct manual : wish<void, "manual">
{
    coin_t token = 0;

    constexpr explicit manual(coin_t token = 0) noexcept
        : token(token)
    {}

    auto args() const
    {
        return wish_args(wish_arg{"token", token});
    }
};

struct openat : wish<int, "openat">
{
    int dirfd = AT_FDCWD;
    std::string path;
    int flags = O_RDONLY;
    mode_t mode = 0;

    explicit openat(
        int dirfd = AT_FDCWD,
        std::string path = {},
        int flags = O_RDONLY,
        mode_t mode = 0)
        : dirfd(dirfd)
        , path(std::move(path))
        , flags(flags)
        , mode(mode)
    {}

    auto args() const
    {
        return path_args(path);
    }
};

#if defined(__linux__)
/// openat2(2). RESOLVE_* flags make the kernel confine resolution, e.g.
/// RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS for paths that must stay inside
/// DIRFD. The wish owns HOW for the lifetime of the operation.
struct openat2 : wish<int, "openat2">
{
    int dirfd = AT_FDCWD;
    std::string path;
    open_how how{};

    explicit openat2(
        int dirfd = AT_FDCWD,
        std::string path = {},
        std::uint64_t flags = O_RDONLY,
        std::uint64_t resolve = 0)
        : dirfd(dirfd)
        , path(std::move(path))
        , how{.flags = flags, .mode = 0, .resolve = resolve}
    {}

    auto args() const
    {
        return path_args(path);
    }
};

struct statx : wish<statx_result, "statx">
{
    int dirfd = AT_FDCWD;
    std::string path;
    int flags = AT_SYMLINK_NOFOLLOW;
    unsigned mask = STATX_BASIC_STATS;
    statx_result result{};

    explicit statx(
        int dirfd = AT_FDCWD,
        std::string path = {},
        int flags = AT_SYMLINK_NOFOLLOW,
        unsigned mask = STATX_BASIC_STATS)
        : dirfd(dirfd)
        , path(std::move(path))
        , flags(flags)
        , mask(mask)
    {}

    auto args() const
    {
        return path_args(path);
    }
};

struct getdents64 : wish<std::size_t, "getdents64">
{
    int fd = -1;
    std::span<std::byte> buffer;

    constexpr explicit getdents64(
        int fd = -1,
        std::span<std::byte> buffer = {}) noexcept
        : fd(fd)
        , buffer(buffer)
    {}

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }
};

#endif

struct spawn_piped : wish<piped_child, "spawn-piped">
{
    std::vector<std::string> argv;
    std::shared_ptr<piped_child> child = std::make_shared<piped_child>();

    explicit spawn_piped(std::vector<std::string> argv = {})
        : argv(std::move(argv))
    {}

    auto args() const
    {
        return wish_args(wish_arg{"argv", argv.size()});
    }
};

struct spawn_pty : wish<pty_child, "spawn-pty">
{
    std::vector<std::string> argv;
    std::size_t columns = 80;
    std::size_t rows = 24;
    std::shared_ptr<pty_child> child = std::make_shared<pty_child>();

    explicit spawn_pty(
        std::vector<std::string> argv = {},
        std::size_t columns = 80,
        std::size_t rows = 24)
        : argv(std::move(argv))
        , columns(columns)
        , rows(rows)
    {}

    auto args() const
    {
        return wish_args(wish_arg{"argv", argv.size()});
    }
};

/// Waits for a child to exit. Linux reaps it here; elsewhere the status is
/// read without reaping and the child_handle reaps it later, so waiting
/// again reports the same status instead of ECHILD.
struct wait_child : wish<child_result, "wait-child">
{
    int child = -1; // child_ref(): a pidfd on Linux, the pid elsewhere
    siginfo_t info{}; // NOLINT(misc-include-cleaner)

    constexpr explicit wait_child(int child = -1) noexcept
        : child(child)
    {}

    auto args() const
    {
        return wish_args(wish_arg{"child", child});
    }
};

struct signal_child : wish<void, "signal-child">
{
    int child = -1; // child_ref(): a pidfd on Linux, the pid elsewhere
    int signal = SIGTERM;

    constexpr explicit signal_child(
        int child = -1,
        int signal = SIGTERM) noexcept
        : child(child)
        , signal(signal)
    {}

    auto args() const
    {
        return wish_args(
            wish_arg{"child", child},
            wish_arg{"signal", signal});
    }
};

struct read_some : wish<std::size_t, "read">
{
    int fd = -1;
    std::span<std::byte> buffer;
    off_t offset = -1;

    constexpr explicit read_some(
        int fd = -1,
        std::span<std::byte> buffer = {},
        off_t offset = -1) noexcept
        : fd(fd)
        , buffer(buffer)
        , offset(offset)
    {}

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }
};

struct write_some : wish<std::size_t, "write">
{
    int fd = -1;
    std::span<const std::byte> buffer;
    off_t offset = -1;

    constexpr explicit write_some(
        int fd = -1,
        std::span<const std::byte> buffer = {},
        off_t offset = -1) noexcept
        : fd(fd)
        , buffer(buffer)
        , offset(offset)
    {}

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }
};

struct recv_some : wish<std::size_t, "recv">
{
    int fd = -1;
    std::span<std::byte> buffer;
    int flags = 0;

    constexpr explicit recv_some(
        int fd = -1,
        std::span<std::byte> buffer = {},
        int flags = 0) noexcept
        : fd(fd)
        , buffer(buffer)
        , flags(flags)
    {}

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }
};

struct send_some : wish<std::size_t, "send">
{
    int fd = -1;
    std::span<const std::byte> buffer;
    int flags = 0;

    constexpr explicit send_some(
        int fd = -1,
        std::span<const std::byte> buffer = {},
        int flags = 0) noexcept
        : fd(fd)
        , buffer(buffer)
        , flags(flags)
    {}

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }
};

struct connect : wish<void, "connect">
{
    int fd = -1;
    sockaddr_storage address{};
    socklen_t address_size = 0; // NOLINT(misc-include-cleaner)

    constexpr explicit connect(
        int fd = -1,
        sockaddr_storage address = {},
        socklen_t address_size = 0) noexcept
        : fd(fd)
        , address(address)
        , address_size(address_size)
    {}

    auto args() const
    {
        return fd_args(fd);
    }

    static connect from(
        int fd,
        sockaddr const * address,
        socklen_t address_size)
    {
        if (address_size > sizeof(sockaddr_storage))
            throw runtime_error{"connect address is too large"};

        auto op = connect{fd, {}, address_size};
        std::memcpy(&op.address, address, address_size);
        return op;
    }

    [[nodiscard]] sockaddr const * sockaddr_ptr() const noexcept
    {
        return reinterpret_cast<sockaddr const *>(&address);
    }
};

/// Accept one connection from a listening socket.
///
/// The accepted file descriptor is returned as the wish result. The caller owns
/// it immediately and should wrap it in an RAII file descriptor type.
struct accept : wish<int, "accept">
{
    int fd = -1;
    int flags = 0;

    constexpr explicit accept(int fd = -1, int flags = 0) noexcept
        : fd(fd)
        , flags(flags)
    {}

    auto args() const
    {
        return fd_args(fd);
    }
};

struct poll : wish<int, "poll">
{
    int fd = -1;
    short events = 0;

    constexpr explicit poll(int fd = -1, short events = 0) noexcept
        : fd(fd)
        , events(events)
    {}

    auto args() const
    {
        return wish_args(
            wish_arg{"fd", fd},
            wish_arg{"events", events});
    }
};

struct timeout : wish<void, "timeout">
{
    kernel_timespec duration{};

    constexpr explicit timeout(kernel_timespec duration = {}) noexcept
        : duration(duration)
    {}

    auto args() const
    {
        return no_args();
    }

    static timeout after(std::chrono::nanoseconds duration)
    {
        return timeout{as_kernel_timespec(duration)};
    }
};

/// Backend-specific fused poll/deadline wish.
///
/// Portable runtime code should prefer `poll_until_after`, which composes a
/// poll wish and timeout wish and lets ordinary task racing choose the winner.
struct poll_until : wish<poll_until_result, "poll-until">
{
    int fd = -1;
    short events = 0;
    kernel_timespec timeout{};

    constexpr explicit poll_until(
        int fd = -1,
        short events = 0,
        kernel_timespec timeout = {}) noexcept
        : fd(fd)
        , events(events)
        , timeout(timeout)
    {}

    auto args() const
    {
        return wish_args(
            wish_arg{"fd", fd},
            wish_arg{"events", events});
    }

    static poll_until after(
        int fd,
        short events,
        std::chrono::nanoseconds timeout)
    {
        return poll_until{fd, events, as_kernel_timespec(timeout)};
    }
};

} // namespace op

using wish_variant = std::variant<
    op::manual,
    op::openat,
#if defined(__linux__)
    op::openat2,
    op::statx,
    op::getdents64,
#endif
    op::spawn_piped,
    op::spawn_pty,
    op::wait_child,
    op::signal_child,
    op::read_some,
    op::write_some,
    op::recv_some,
    op::send_some,
    op::connect,
    op::accept,
    op::poll,
    op::timeout,
    op::poll_until>;

} // namespace nxtrt
