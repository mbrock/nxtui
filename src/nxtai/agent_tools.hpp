#pragma once

// Extended tools for nxtllm: filesystem reading, ripgrep search,
// and web fetch (via `lightpanda fetch --dump markdown`).
//
// Subprocess-backed tools run blocking work on a std::async thread
// and yield the scheduler periodically so the per-tool spinner card
// in tool_ui.hpp animates while the work is in flight.

#include <nxtai/tools.hpp>
#include <nxtio/async-core.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <spawn.h>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" char ** environ;

namespace nxt::ai::agent_tools {

inline std::string read_file_to_string(
    const std::filesystem::path & p, std::size_t max_bytes)
{
    auto file = std::ifstream{p, std::ios::binary};
    if (!file.is_open())
        return {};
    std::string out;
    out.resize(max_bytes);
    file.read(out.data(), static_cast<std::streamsize>(max_bytes));
    out.resize(static_cast<std::size_t>(file.gcount()));
    return out;
}

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

// Spawn `argv[0]` with the rest as arguments, redirecting both
// stdout and stderr into a non-blocking pipe. Read from the pipe
// asynchronously via `scheduler.poll` so the coroutine yields to
// the scheduler between bursts of output and any sibling spinner
// keeps animating. Returns when the child closes its end (EOF), the
// output cap is hit, or the stop token fires (in which case we send
// SIGTERM and wait for it).
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
    // Children inherit fds without O_CLOEXEC, but pipe2 set CLOEXEC
    // on both. Clear it for the child's stdout/stderr.
    ::posix_spawn_file_actions_adddup2(
        &actions, pipefd[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(
        &actions, pipefd[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    ::posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    std::vector<char *> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (auto & a : argv)
        ptrs.push_back(a.data());
    ptrs.push_back(nullptr);

    pid_t pid = 0;
    auto rc = ::posix_spawnp(
        &pid,
        argv[0].c_str(),
        &actions,
        nullptr,
        ptrs.data(),
        environ);
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
        if (status == nxt::poll_status::closed
            || status == nxt::poll_status::error)
            break;

        while (true) {
            std::array<char, 4096> buf{};
            auto n = ::read(pipefd[0], buf.data(), buf.size());
            if (n > 0) {
                out.append(
                    buf.data(),
                    static_cast<std::size_t>(n));
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
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            done = true;
            break;
        }
    }

    ::close(pipefd[0]);
    int wstatus = 0;
    // Reap. If we asked it to terminate, give it a moment, then
    // SIGKILL if needed. waitpid is unfortunately blocking, but
    // child should be gone almost immediately after pipe EOF.
    for (int attempt = 0; attempt < 50; ++attempt) {
        auto r = ::waitpid(pid, &wstatus, WNOHANG);
        if (r > 0)
            break;
        if (r == 0) {
            if (attempt == 10)
                ::kill(pid, SIGKILL);
            co_await sched.yield_for(
                std::chrono::milliseconds{10});
            continue;
        }
        break;
    }
    co_return out;
}

struct read_file_tool
{
    static constexpr std::string_view name = "read_file";
    static constexpr std::string_view description =
        "Read a text file from the local filesystem. Returns "
        "JSON with the file path, byte count, and the file's "
        "contents truncated to 80 KiB.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string path;

        struct glaze_json_schema
        {
            glz::schema path{
                .description = "Absolute or relative path to the file"};
        };
    };

    struct result
    {
        std::string path;
        std::size_t bytes = 0;
        std::string contents;
    };

    struct error_result
    {
        std::string error;
        std::string path;
    };

    static std::string parameters_summary(const parameters & args)
    {
        return args.path;
    }

    nxt::task<std::string> run(parameters args) const
    {
        auto path = std::move(args.path);
        if (path.empty()) {
            co_return glz::ex::write_json(error_result{
                .error = "missing required parameter `path`",
                .path = {},
            });
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            co_return glz::ex::write_json(error_result{
                .error = "file does not exist",
                .path = path,
            });
        }
        auto contents = read_file_to_string(path, 80 * 1024);
        co_return glz::ex::write_json(result{
            .path = std::move(path),
            .bytes = contents.size(),
            .contents = std::move(contents),
        });
    }
};

struct rg_search_tool
{
    static constexpr std::string_view name = "rg_search";
    static constexpr std::string_view description =
        "Search for a regex pattern across files using ripgrep. "
        "Returns JSON with the matching lines (file:line:text). "
        "Use this to locate symbols, definitions, or usages.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string pattern;
        std::string path = ".";

        struct glaze_json_schema
        {
            glz::schema pattern{
                .description = "Regex pattern to search for (rg-style)"};
            glz::schema path{
                .description =
                    "Directory or file to search; use \".\" for current working directory"};
        };
    };

    struct result
    {
        std::string pattern;
        std::string path;
        std::size_t bytes = 0;
        std::string output;
    };

    struct error_result
    {
        std::string error;
    };

    static std::string parameters_summary(const parameters & args)
    {
        auto path = args.path.empty() ? std::string{"."} : args.path;
        return "/" + args.pattern + "/ in " + path;
    }

    nxt::scheduler * sched = nullptr;

    nxt::task<std::string> run(parameters args) const
    {
        if (args.pattern.empty()) {
            co_return glz::ex::write_json(error_result{
                .error = "missing pattern",
            });
        }
        if (args.path.empty())
            args.path = ".";
        auto pattern = args.pattern;
        auto path = args.path;
        std::vector<std::string> argv = {
            "rg",
            "--no-heading",
            "--line-number",
            "--max-count",
            "50",
            "--max-columns",
            "200",
            "--",
            pattern,
            path,
        };
        auto output = co_await run_subprocess_async(
            *sched, std::move(argv), 60 * 1024);
        co_return glz::ex::write_json(result{
            .pattern = std::move(args.pattern),
            .path = std::move(args.path),
            .bytes = output.size(),
            .output = std::move(output),
        });
    }
};

struct web_fetch_tool
{
    static constexpr std::string_view name = "web_fetch";
    static constexpr std::string_view description =
        "Fetch a URL and return its content as Markdown using "
        "lightpanda (a headless browser). Useful for reading "
        "documentation pages, blog posts, or any public web "
        "content. Renders JS before extracting.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string url;

        struct glaze_json_schema
        {
            glz::schema url{.description = "HTTPS URL to fetch"};
        };
    };

    struct result
    {
        std::string url;
        std::size_t bytes = 0;
        std::string output;
    };

    struct error_result
    {
        std::string error;
    };

    static std::string parameters_summary(const parameters & args)
    {
        return args.url;
    }

    nxt::scheduler * sched = nullptr;

    nxt::task<std::string> run(parameters args) const
    {
        if (args.url.empty()) {
            co_return glz::ex::write_json(error_result{
                .error = "missing url",
            });
        }
        auto url = args.url;
        std::vector<std::string> argv = {
            "lightpanda",
            "fetch",
            "--dump",
            "markdown",
            "--strip-mode",
            "full",
            url,
        };
        auto output = co_await run_subprocess_async(
            *sched, std::move(argv), 120 * 1024);
        co_return glz::ex::write_json(result{
            .url = std::move(args.url),
            .bytes = output.size(),
            .output = std::move(output),
        });
    }
};

struct bash_tool
{
    static constexpr std::string_view name = "bash";
    static constexpr std::string_view description =
        "Run a bash command. The combined stdout+stderr is "
        "returned. This tool REQUIRES user approval — the user "
        "will be prompted to confirm or deny before the command "
        "runs. Use it for read-only inspections and idempotent "
        "operations; avoid destructive commands.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string command;

        struct glaze_json_schema
        {
            glz::schema command{
                .description =
                    "Full shell command line. Will be passed to /bin/bash -c."};
        };
    };

    struct result
    {
        std::string command;
        std::size_t bytes = 0;
        std::string output;
    };

    struct error_result
    {
        std::string error;
    };

    static std::string parameters_summary(const parameters & args)
    {
        return args.command;
    }

    nxt::scheduler * sched = nullptr;

    nxt::task<std::string> run(parameters args) const
    {
        if (args.command.empty()) {
            co_return glz::ex::write_json(error_result{
                .error = "missing command",
            });
        }
        auto command = args.command;
        std::vector<std::string> argv = {
            "/bin/bash", "-c", command};
        auto output = co_await run_subprocess_async(
            *sched, std::move(argv), 80 * 1024);
        co_return glz::ex::write_json(result{
            .command = std::move(args.command),
            .bytes = output.size(),
            .output = std::move(output),
        });
    }
};

inline auto for_agent(nxt::scheduler & sched)
{
    return tools::tool_set{
        read_file_tool{},
        rg_search_tool{.sched = &sched},
        web_fetch_tool{.sched = &sched},
        bash_tool{.sched = &sched},
    };
}

} // namespace nxt::ai::agent_tools
