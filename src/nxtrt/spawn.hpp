#pragma once

#include <nxt/unique-fd.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char ** environ; // NOLINT(readability-redundant-declaration)
#endif

/// Synchronous child creation shared by the wands' spawn wishes. Each
/// function returns 0 or a negated errno and never leaves a child behind on
/// failure. Wands wrap the pid in their own child handle (a pidfd on Linux).
///
/// Children get the parent's environment and search PATH for ARGV[0].
/// `piped` children on macOS inherit only the descriptors set up here
/// (`POSIX_SPAWN_CLOEXEC_DEFAULT`). Other children (`piped` elsewhere, and
/// `pty`, which forks) also inherit any parent descriptor not marked
/// close-on-exec, so open descriptors with O_CLOEXEC. Task code should use
/// `nxtrt::subprocess` or `nxtrt::pty` rather than these functions.
namespace nxtrt::spawn {

/// A started child: its pid and the parent's end of its output.
struct spawned
{
    pid_t pid = -1;
    nxt::unique_fd fd{}; // merged stdout/stderr pipe, or PTY master
};

namespace detail {

inline char ** environment() noexcept
{
#if defined(__APPLE__)
    return *::_NSGetEnviron();
#else
    return environ;
#endif
}

inline void set_cloexec(int fd) noexcept
{
    auto flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0)
        (void) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// A child's stdio is dup2'd from these, so they must not be 0-2 when the
// parent runs with closed stdio.
inline bool move_fd_above_stdio(int & fd) noexcept
{
    if (fd > STDERR_FILENO)
        return true;
    auto replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (replacement < 0)
        return false;
    ::close(fd);
    fd = replacement;
    return true;
}

inline int cloexec_pipe(nxt::unique_fd & read, nxt::unique_fd & write)
{
    auto fds = std::array<int, 2>{-1, -1};
#if defined(__linux__)
    if (::pipe2(fds.data(), O_CLOEXEC) != 0)
        return -errno;
#else
    // No pipe2; the window before FD_CLOEXEC only matters for children
    // started without POSIX_SPAWN_CLOEXEC_DEFAULT (the PTY fork below).
    if (::pipe(fds.data()) != 0)
        return -errno;
    set_cloexec(fds[0]);
    set_cloexec(fds[1]);
#endif
    read.reset(fds[0]);
    write.reset(fds[1]);
    auto r = read.release(), w = write.release();
    auto ok = move_fd_above_stdio(r) && move_fd_above_stdio(w);
    auto error = errno;
    read.reset(r);
    write.reset(w);
    return ok ? 0 : -error;
}

inline std::vector<char *> argv_pointers(std::vector<std::string> & argv)
{
    auto pointers = std::vector<char *>{};
    pointers.reserve(argv.size() + 1);
    for (auto & arg : argv)
        pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    return pointers;
}

class file_actions
{
public:
    file_actions() noexcept
        : rc_(::posix_spawn_file_actions_init(&actions_))
    {}
    file_actions(const file_actions &) = delete;
    file_actions & operator=(const file_actions &) = delete;
    ~file_actions()
    {
        if (initialized_)
            ::posix_spawn_file_actions_destroy(&actions_);
    }

    // Records the first failure; later calls become no-ops.
    template<typename Fn, typename... Args>
    void add(Fn fn, Args... args) noexcept
    {
        if (rc_ == 0)
            rc_ = fn(&actions_, args...);
    }

    [[nodiscard]] int error() const noexcept
    {
        return rc_ == 0 ? 0 : -rc_;
    }

    [[nodiscard]] posix_spawn_file_actions_t * get() noexcept
    {
        return &actions_;
    }

private:
    posix_spawn_file_actions_t actions_{};
    int rc_ = 0;
    bool initialized_ = rc_ == 0;
};

class attributes
{
public:
    attributes() noexcept
        : rc_(::posix_spawnattr_init(&attributes_))
    {
#if defined(__APPLE__)
        // Inherit nothing but the descriptors the file actions name: no
        // other thread's descriptor can leak into the child.
        if (rc_ == 0)
            rc_ = ::posix_spawnattr_setflags(
                &attributes_, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    }
    attributes(const attributes &) = delete;
    attributes & operator=(const attributes &) = delete;
    ~attributes()
    {
        if (initialized_)
            ::posix_spawnattr_destroy(&attributes_);
    }

    [[nodiscard]] int error() const noexcept
    {
        return rc_ == 0 ? 0 : -rc_;
    }

    [[nodiscard]] posix_spawnattr_t * get() noexcept
    {
        return &attributes_;
    }

private:
    posix_spawnattr_t attributes_{};
    int rc_ = 0;
    bool initialized_ = rc_ == 0;
};

} // namespace detail

/// Runs ARGV (PATH-searched) with stdin from /dev/null and stdout and
/// stderr merged into one pipe, whose read end becomes OUT.fd.
inline int piped(std::vector<std::string> & argv, spawned & out)
{
    if (argv.empty())
        return -EINVAL;
    auto read = nxt::unique_fd{}, write = nxt::unique_fd{};
    if (auto rc = detail::cloexec_pipe(read, write); rc < 0)
        return rc;

    auto actions = detail::file_actions{};
    actions.add(
        ::posix_spawn_file_actions_addopen,
        STDIN_FILENO,
        "/dev/null",
        O_RDONLY,
        mode_t{0});
    actions.add(
        ::posix_spawn_file_actions_adddup2, write.get(), STDOUT_FILENO);
    actions.add(
        ::posix_spawn_file_actions_adddup2, write.get(), STDERR_FILENO);
    actions.add(::posix_spawn_file_actions_addclose, read.get());
    actions.add(::posix_spawn_file_actions_addclose, write.get());
    if (auto rc = actions.error(); rc < 0)
        return rc;
    auto attributes = detail::attributes{};
    if (auto rc = attributes.error(); rc < 0)
        return rc;

    auto pointers = detail::argv_pointers(argv);
    auto pid = pid_t{-1};
    auto rc = ::posix_spawnp(
        &pid,
        pointers[0],
        actions.get(),
        attributes.get(),
        pointers.data(),
        detail::environment());
    if (rc != 0)
        return -rc;
    out = spawned{.pid = pid, .fd = std::move(read)};
    return 0;
}

/// Runs ARGV as a session leader whose controlling terminal is a new PTY
/// of COLUMNS x ROWS; the master becomes OUT.fd. Exit status 127 means
/// ARGV[0] was not found, 126 that the child could not be set up.
inline int pty(
    std::vector<std::string> & argv,
    std::size_t columns,
    std::size_t rows,
    spawned & out)
{
    if (argv.empty())
        return -EINVAL;
    auto master = nxt::unique_fd{::posix_openpt(O_RDWR | O_NOCTTY)};
    if (master.get() < 0)
        return -errno;
    detail::set_cloexec(master.get());
    if (::grantpt(master.get()) < 0 || ::unlockpt(master.get()) < 0)
        return -errno;
    auto name = std::array<char, 256>{};
    if (::ptsname_r(master.get(), name.data(), name.size()) != 0)
        return -errno;
    auto slave = nxt::unique_fd{
        ::open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
    if (slave.get() < 0)
        return -errno;
    auto size = winsize{
        .ws_row = static_cast<unsigned short>(std::max<std::size_t>(1, rows)),
        .ws_col =
            static_cast<unsigned short>(std::max<std::size_t>(1, columns)),
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    if (::ioctl(slave.get(), TIOCSWINSZ, &size) < 0)
        return -errno;

    // posix_spawn cannot acquire a controlling terminal portably. The child
    // only makes async-signal-safe calls before exec.
    auto pointers = detail::argv_pointers(argv);
    auto pid = ::fork();
    if (pid < 0)
        return -errno;
    if (pid == 0) {
        ::close(master.get());
        if (::setsid() < 0 || ::ioctl(slave.get(), TIOCSCTTY, 0) < 0)
            ::_exit(126);
        ::dup2(slave.get(), STDIN_FILENO);
        ::dup2(slave.get(), STDOUT_FILENO);
        ::dup2(slave.get(), STDERR_FILENO);
        if (slave.get() > STDERR_FILENO)
            ::close(slave.get());
        ::execvp(pointers[0], pointers.data());
        ::_exit(errno == ENOENT ? 127 : 126);
    }
    out = spawned{.pid = pid, .fd = std::move(master)};
    return 0;
}

/// Kills and reaps a child that a wand could not wrap in its handle.
inline void abandon(pid_t pid) noexcept
{
    ::kill(pid, SIGKILL);
    (void) ::waitpid(pid, nullptr, 0);
}

} // namespace nxtrt::spawn
