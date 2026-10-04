#pragma once

#include "wish.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include "nxt/unique-fd.hpp"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
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

#if defined(_WIN32)
/// Native overlapped file/pipe handle; not a CRT descriptor.
using io_handle = HANDLE;
/// Native Winsock socket; never narrowed to an int.
using socket_handle = SOCKET;
using socket_length = int;
using file_offset = std::int64_t;
inline constexpr io_handle invalid_io_handle = nullptr;
inline constexpr socket_handle invalid_socket_handle = INVALID_SOCKET;
#else
using io_handle = int;
using socket_handle = int;
using socket_length = socklen_t;
using file_offset = off_t;
inline constexpr io_handle invalid_io_handle = -1;
inline constexpr socket_handle invalid_socket_handle = -1;
#endif

#if defined(__linux__)
/// Seconds-plus-nanoseconds duration carried by timer wishes.
using kernel_timespec = __kernel_timespec;
#else
/// Seconds-plus-nanoseconds duration carried by timer wishes.
struct kernel_timespec
{
    std::int64_t tv_sec = 0;
    std::int64_t tv_nsec = 0;
};
#endif

/// Converts a duration to a `kernel_timespec`, clamping negatives to zero.
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
/// Result of the `statx` wish: the kernel's `struct statx`.
using statx_result = struct statx;
#endif

#if !defined(_WIN32)
/// Exit status reported by the `wait_child` wish.
///
/// `code` is the raw `siginfo_t::si_code`. `exited`/`exit_code` are set for
/// a normal exit; `signaled`/`signal` for a child killed by a signal
/// (including a core dump).
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
#endif // !defined(_WIN32)

namespace op {

/// Test wish with no platform effect; yields `void`.
///
/// Test wands key their parked tasks by `token` and complete them when the
/// test says so. The shipped wands complete it at once (io_uring as a NOP
/// SQE). Like every wish, it owns its inputs and names its result type.
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

#if !defined(_WIN32)
/// Opens `path` relative to `dirfd` with openat(2); yields the new fd.
///
/// The caller owns the returned descriptor. io_uring opens it in the
/// kernel; epoll and kqueue call openat(2) on the deck's thread during
/// `wave`, so a slow disk blocks the deck (see `wand::asynchronous_files`).
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
/// Opens `path` with openat2(2) (Linux only); yields the new fd.
///
/// RESOLVE_* flags make the kernel confine resolution, e.g.
/// RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS for paths that must stay inside
/// `dirfd`. The wish owns `how` for the lifetime of the operation. Kernel
/// asynchronous on io_uring, a synchronous syscall on epoll.
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

/// Stats `path` relative to `dirfd` (Linux only); yields `statx_result`.
///
/// Kernel asynchronous on io_uring, a synchronous syscall on epoll.
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

/// Reads directory entries from `fd` into `buffer` (Linux only); yields
/// the number of bytes filled, zero at the end of the directory.
///
/// Every wand runs getdents64(2) synchronously on the deck's thread;
/// io_uring has no getdents operation.
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

/// Spawns `argv` with stdout and stderr on one pipe; yields `piped_child`.
///
/// Spawn wishes run `posix_spawn` synchronously during `wave` on every
/// wand; see `nxtrt/spawn.hpp`. `argv[0]` is looked up in `PATH`. On Linux
/// the child is adopted as a pidfd before the result is delivered.
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

/// Spawns `argv` as session leader on a new PTY of `columns` x `rows`;
/// yields `pty_child`, which holds the master side.
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

/// Sends `signal` to a child named by `child_ref()`.
///
/// Linux uses pidfd_send_signal(2); elsewhere kill(2) on the still-unreaped
/// pid. Runs synchronously during `wave`.
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
#endif // !defined(_WIN32)

/// Reads at most `buffer.size()` bytes from `fd`; yields the count read,
/// zero at end of file.
///
/// `offset < 0` reads at the file position (read(2)); otherwise pread(2) at
/// `offset`. `buffer` is borrowed until the `co_await` returns. epoll and
/// kqueue switch `fd` to `O_NONBLOCK`, try the syscall at once, and wait
/// for readiness only on `EAGAIN`.
struct read_some : wish<std::size_t, "read">
{
    io_handle fd = invalid_io_handle;
    std::span<std::byte> buffer;
    file_offset offset = -1;

    constexpr explicit read_some(
        io_handle fd = invalid_io_handle,
        std::span<std::byte> buffer = {},
        file_offset offset = -1) noexcept
        : fd(fd)
        , buffer(buffer)
        , offset(offset)
    {}

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }
};

/// Writes at most `buffer.size()` bytes to `fd`; yields the count written.
///
/// Offset, buffer lifetime, and `O_NONBLOCK` behave as for `read_some`.
struct write_some : wish<std::size_t, "write">
{
    io_handle fd = invalid_io_handle;
    std::span<const std::byte> buffer;
    file_offset offset = -1;

    constexpr explicit write_some(
        io_handle fd = invalid_io_handle,
        std::span<const std::byte> buffer = {},
        file_offset offset = -1) noexcept
        : fd(fd)
        , buffer(buffer)
        , offset(offset)
    {}

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }
};

/// recv(2) on socket `fd` with `flags`; yields the count received, zero
/// when the peer has shut down.
struct recv_some : wish<std::size_t, "recv">
{
    socket_handle fd = invalid_socket_handle;
    std::span<std::byte> buffer;
    int flags = 0;

    constexpr explicit recv_some(
        socket_handle fd = invalid_socket_handle,
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

/// send(2) on socket `fd` with `flags`; yields the count sent.
struct send_some : wish<std::size_t, "send">
{
    socket_handle fd = invalid_socket_handle;
    std::span<const std::byte> buffer;
    int flags = 0;

    constexpr explicit send_some(
        socket_handle fd = invalid_socket_handle,
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

/// Connects socket `fd` to `address`; completes when connected.
///
/// Build it with `connect::from(fd, sockaddr, length)`, which copies the
/// address into the wish and throws `runtime_error` if it does not fit in
/// `sockaddr_storage`. epoll and kqueue make `fd` non-blocking and report
/// the final `SO_ERROR` as an `errno_error`.
struct connect : wish<void, "connect">
{
    socket_handle fd = invalid_socket_handle;
    sockaddr_storage address{};
    socket_length address_size = 0;

    constexpr explicit connect(
        socket_handle fd = invalid_socket_handle,
        sockaddr_storage address = {},
        socket_length address_size = 0) noexcept
        : fd(fd)
        , address(address)
        , address_size(address_size)
    {}

    auto args() const
    {
        return fd_args(fd);
    }

    static connect from(
        socket_handle fd,
        sockaddr const * address,
        socket_length address_size)
    {
        if (static_cast<std::size_t>(address_size) > sizeof(sockaddr_storage))
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
/// `flags` are accept4(2) flags such as `SOCK_CLOEXEC`; the epoll wand
/// always adds `SOCK_CLOEXEC | SOCK_NONBLOCK`.
struct accept : wish<socket_handle, "accept">
{
    socket_handle fd = invalid_socket_handle;
    int flags = 0;

    constexpr explicit accept(
        socket_handle fd = invalid_socket_handle, int flags = 0) noexcept
        : fd(fd)
        , flags(flags)
    {}

    auto args() const
    {
        return fd_args(fd);
    }
};

/// Waits once until `fd` is ready for `events` (`POLLIN`, `POLLOUT`, ...);
/// yields the ready poll bits.
///
/// The reported bits differ by wand: io_uring and epoll include
/// `POLLERR`/`POLLHUP`; kqueue reports only `POLLIN` or `POLLOUT`. With
/// kqueue, at most one wish should wait on the same fd and direction at a
/// time, since kqueue keys registrations by fd and filter.
struct poll : wish<int, "poll">
{
    socket_handle fd = invalid_socket_handle;
    short events = 0;

    constexpr explicit poll(
        socket_handle fd = invalid_socket_handle, short events = 0) noexcept
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

/// Completes after `duration` has elapsed on a monotonic clock.
///
/// Build it with `timeout::after(duration)`; negative durations count as
/// zero. Stopping the waiting task cancels the timer and throws
/// `operation_cancelled` from the `co_await`.
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
/// epoll and kqueue implement this wish directly; io_uring rejects it with
/// `runtime_error`.
struct poll_until : wish<poll_until_result, "poll-until">
{
    socket_handle fd = invalid_socket_handle;
    short events = 0;
    kernel_timespec timeout{};

    constexpr explicit poll_until(
        socket_handle fd = invalid_socket_handle,
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
        socket_handle fd,
        short events,
        std::chrono::nanoseconds timeout)
    {
        return poll_until{fd, events, as_kernel_timespec(timeout)};
    }
};

} // namespace op

/// Closed set of every wish a wand must be able to realize.
///
/// `wand::prepare` erases a wish into this variant before handing it to the
/// backend, so a new wish type must be listed here.
using wish_variant = std::variant<
    op::manual,
#if !defined(_WIN32)
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
#endif
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
