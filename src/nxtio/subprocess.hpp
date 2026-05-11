#pragma once

#include "nxt/units.hpp"
#include "nxt/vterm.hpp"
#include "nxtio/async-core.hpp"
#include "nxtio/input.hpp"

#include <csignal>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace nxt::subprocess {

/// Decoded exit status for a child process.
struct ExitStatus
{
    /// Child process id.
    pid_t pid = -1;
    /// Raw status word returned by `waitpid`.
    int raw_status = 0;
    /// True when the process exited normally.
    bool exited = false;
    /// True when the process was terminated by a signal.
    bool signaled = false;
    /// Exit code when `exited` is true.
    int exit_code = -1;
    /// Terminating signal when `signaled` is true.
    int signal = 0;

    /// True for a normal zero exit code.
    [[nodiscard]] bool success() const noexcept
    {
        return exited && exit_code == 0;
    }

    /// Human-readable status summary.
    [[nodiscard]] std::string describe() const;
};

/// Options for spawning a child attached to a pseudo-terminal.
struct SpawnOptions
{
    /// Executable and arguments; `argv[0]` is the program to run.
    std::vector<std::string> argv;
    /// Optional working directory for the child.
    std::optional<std::filesystem::path> cwd;
    /// Complete environment. Empty means inherit the parent environment.
    std::vector<std::string> environment;
    /// Environment entries that override or extend the base environment.
    std::vector<std::string> environment_overlay;
    /// Initial PTY size in terminal cells.
    Size size{80 * ch, 24 * ln};
};

/// Running child process connected to a PTY and a libvterm screen model.
class PtySession
{
public:
    /// Called after input bytes from the PTY changed the terminal screen.
    using DamageCallback = std::function<void()>;

    /// Spawn a child process attached to a new PTY.
    static PtySession spawn(const SpawnOptions & options);
    /// Spawn an interactive shell attached to a new PTY.
    static PtySession shell(
        Size size,
        std::string shell_path = {},
        std::optional<std::filesystem::path> cwd = std::nullopt);

    ~PtySession();

    PtySession(const PtySession &) = delete;
    PtySession & operator=(const PtySession &) = delete;
    PtySession(PtySession && other) noexcept;
    PtySession & operator=(PtySession && other) noexcept;

    /// Child process id, or -1 after the session has been moved from.
    [[nodiscard]] pid_t child_pid() const noexcept
    {
        return child_pid_;
    }

    /// PTY master file descriptor, or -1 when closed/moved.
    [[nodiscard]] int master_fd() const noexcept
    {
        return master_fd_;
    }

    /// True while the child exists and has not been reaped.
    [[nodiscard]] bool running() const noexcept
    {
        return child_pid_ > 0 && !exit_status_.has_value();
    }

    /// Last observed exit status, if the child has been reaped.
    [[nodiscard]] std::optional<ExitStatus> exit_status() const
    {
        return exit_status_;
    }

    /// Terminal screen model owned by the session.
    [[nodiscard]] nxt::vterm::Terminal & terminal() noexcept
    {
        return terminal_;
    }

    /// Const terminal screen model owned by the session.
    [[nodiscard]] const nxt::vterm::Terminal & terminal() const noexcept
    {
        return terminal_;
    }

    /// Run a function while holding the terminal mutex.
    template<typename Fn>
    decltype(auto) with_terminal(Fn && fn)
    {
        std::scoped_lock lock{terminal_mutex_};
        return std::forward<Fn>(fn)(terminal_);
    }

    /// Run a function while holding the terminal mutex.
    template<typename Fn>
    decltype(auto) with_terminal(Fn && fn) const
    {
        std::scoped_lock lock{terminal_mutex_};
        return std::forward<Fn>(fn)(terminal_);
    }

    /// Resize both the kernel PTY and libvterm screen model.
    void resize(Size size);
    /// Send a signal to the child process.
    void terminate(int signal = SIGHUP) noexcept;
    /// Close the PTY master file descriptor.
    void close_master() noexcept;

    /// Write bytes to the PTY master immediately.
    void write(std::string_view bytes);
    /// Asynchronously write all bytes to the PTY master.
    nxt::task<> write_all(
        nxt::scheduler & scheduler,
        std::string bytes,
        std::stop_token stop = {});

    /// Convert a decoded input event into bytes for the child process.
    [[nodiscard]] std::string encode_key(const nxt::input::KeyEvent & event);
    /// Encode and send one input event to the child process.
    nxt::task<> send_key(
        nxt::scheduler & scheduler,
        const nxt::input::KeyEvent & event,
        std::stop_token stop = {});

    /// Read PTY output into libvterm until EOF, cancellation, or process exit.
    nxt::task<ExitStatus> read_loop(
        nxt::scheduler & scheduler,
        std::stop_token stop = {},
        DamageCallback on_damage = {});

private:
    PtySession(int master_fd, pid_t child_pid, Size size);

    [[nodiscard]] std::optional<ExitStatus> try_reap() noexcept;
    [[nodiscard]] nxt::task<ExitStatus> finish_reap(
        nxt::scheduler & scheduler,
        std::stop_token stop);

    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    Size size_{80 * ch, 24 * ln};
    nxt::vterm::Terminal terminal_;
    std::optional<ExitStatus> exit_status_;
    mutable std::mutex terminal_mutex_;
};

} // namespace nxt::subprocess
