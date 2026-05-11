#pragma once

#include "nxt/units.hpp"
#include "nxt/vterm.hpp"
#include "nxtio/async.hpp"
#include "nxtio/input.hpp"

#include <csignal>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace nxt::subprocess {

struct ExitStatus
{
    pid_t pid = -1;
    int raw_status = 0;
    bool exited = false;
    bool signaled = false;
    int exit_code = -1;
    int signal = 0;

    [[nodiscard]] bool success() const noexcept
    {
        return exited && exit_code == 0;
    }

    [[nodiscard]] std::string describe() const;
};

struct SpawnOptions
{
    std::vector<std::string> argv;
    std::optional<std::filesystem::path> cwd;
    std::vector<std::string> environment;
    std::vector<std::string> environment_overlay;
    Size size{80 * ch, 24 * ln};
};

class PtySession
{
public:
    using DamageCallback = std::function<void()>;

    static PtySession spawn(const SpawnOptions & options);
    static PtySession shell(
        Size size,
        std::string shell_path = {},
        std::optional<std::filesystem::path> cwd = std::nullopt);

    ~PtySession();

    PtySession(const PtySession &) = delete;
    PtySession & operator=(const PtySession &) = delete;
    PtySession(PtySession && other) noexcept;
    PtySession & operator=(PtySession && other) noexcept;

    [[nodiscard]] pid_t child_pid() const noexcept
    {
        return child_pid_;
    }

    [[nodiscard]] int master_fd() const noexcept
    {
        return master_fd_;
    }

    [[nodiscard]] bool running() const noexcept
    {
        return child_pid_ > 0 && !exit_status_.has_value();
    }

    [[nodiscard]] std::optional<ExitStatus> exit_status() const
    {
        return exit_status_;
    }

    [[nodiscard]] nxt::vterm::Terminal & terminal() noexcept
    {
        return terminal_;
    }

    [[nodiscard]] const nxt::vterm::Terminal & terminal() const noexcept
    {
        return terminal_;
    }

    template<typename Fn>
    decltype(auto) with_terminal(Fn && fn)
    {
        std::scoped_lock lock{terminal_mutex_};
        return std::forward<Fn>(fn)(terminal_);
    }

    template<typename Fn>
    decltype(auto) with_terminal(Fn && fn) const
    {
        std::scoped_lock lock{terminal_mutex_};
        return std::forward<Fn>(fn)(terminal_);
    }

    void resize(Size size);
    void terminate(int signal = SIGHUP) noexcept;
    void close_master() noexcept;

    void write(std::string_view bytes);
    nxt::task<> write_all(
        nxt::io_scheduler & scheduler,
        std::string bytes,
        std::stop_token stop = {});

    [[nodiscard]] std::string encode_key(const nxt::input::KeyEvent & event);
    nxt::task<> send_key(
        nxt::io_scheduler & scheduler,
        const nxt::input::KeyEvent & event,
        std::stop_token stop = {});

    nxt::task<ExitStatus> read_loop(
        nxt::io_scheduler & scheduler,
        std::stop_token stop = {},
        DamageCallback on_damage = {});

private:
    PtySession(int master_fd, pid_t child_pid, Size size);

    [[nodiscard]] std::optional<ExitStatus> try_reap() noexcept;
    [[nodiscard]] nxt::task<ExitStatus> finish_reap(
        nxt::io_scheduler & scheduler,
        std::stop_token stop);

    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    Size size_{80 * ch, 24 * ln};
    nxt::vterm::Terminal terminal_;
    std::optional<ExitStatus> exit_status_;
    mutable std::mutex terminal_mutex_;
};

} // namespace nxt::subprocess
