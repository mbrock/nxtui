#pragma once

// Shared subprocess support for agent tools. Output is read through the
// scheduler so tool UI spinners can keep animating while a child runs.

#include <nxtio/async-core.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <stop_token>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" char ** environ;

namespace nxt::ai::agent_tools {

inline bool set_fd_flag(int fd, int flag)
{
    auto flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | flag) == 0;
}

inline bool set_status_flag(int fd, int flag)
{
    auto flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | flag) == 0;
}

inline bool make_nonblocking_cloexec_pipe(int pipefd[2])
{
#if defined(__linux__)
    return ::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0;
#else
    if (::pipe(pipefd) != 0)
        return false;
    if (!set_status_flag(pipefd[0], O_NONBLOCK)
        || !set_status_flag(pipefd[1], O_NONBLOCK)
        || !set_fd_flag(pipefd[0], FD_CLOEXEC)
        || !set_fd_flag(pipefd[1], FD_CLOEXEC)) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        pipefd[0] = -1;
        pipefd[1] = -1;
        return false;
    }
    return true;
#endif
}

inline nxt::task<std::string>
run_subprocess_async(
    nxt::scheduler & sched,
    std::vector<std::string> argv,
    std::size_t cap_bytes = 64 * 1024,
    std::stop_token stop = {})
{
    if (argv.empty())
        co_return std::string{};

    int pipefd[2] = {-1, -1};
    if (!make_nonblocking_cloexec_pipe(pipefd))
        co_return std::string{"<pipe() failed>"};

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        co_return std::string{"<spawn init failed>"};
    }
    ::posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    ::posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    std::vector<char *> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (auto & a : argv)
        ptrs.push_back(a.data());
    ptrs.push_back(nullptr);

    pid_t pid = 0;
    auto rc = ::posix_spawnp(
        &pid, argv[0].c_str(), &actions, nullptr, ptrs.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipefd[1]);

    if (rc != 0) {
        ::close(pipefd[0]);
        co_return std::string{"<failed to spawn: "}
            + std::string{std::strerror(rc)} + ">";
    }

    std::string out;
    bool done = false;
    while (!done) {
        if (stop.stop_requested()) {
            ::kill(pid, SIGTERM);
            break;
        }
        auto status = co_await sched.poll(
            pipefd[0],
            nxt::poll_op::read,
            std::chrono::milliseconds{200});
        if (status == nxt::poll_status::timeout)
            continue;
        auto pipe_finished =
            status == nxt::poll_status::closed
            || status == nxt::poll_status::error
            || status == nxt::poll_status::cancelled;

        while (true) {
            std::array<char, 4096> buf{};
            auto n = ::read(pipefd[0], buf.data(), buf.size());
            if (n > 0) {
                out.append(buf.data(), static_cast<std::size_t>(n));
                if (out.size() > cap_bytes) {
                    out.resize(cap_bytes);
                    out += "\n…(output truncated)\n";
                    done = true;
                    break;
                }
                continue;
            }
            if (n == 0) {
                done = true;
                break;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (pipe_finished)
                    done = true;
                break;
            }
            done = true;
            break;
        }
        if (pipe_finished)
            done = true;
    }

    ::close(pipefd[0]);
    int wstatus = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        auto r = ::waitpid(pid, &wstatus, WNOHANG);
        if (r > 0)
            break;
        if (r == 0) {
            if (attempt == 10)
                ::kill(pid, SIGKILL);
            co_await sched.yield_for(std::chrono::milliseconds{10});
            continue;
        }
        break;
    }
    co_return out;
}

} // namespace nxt::ai::agent_tools
