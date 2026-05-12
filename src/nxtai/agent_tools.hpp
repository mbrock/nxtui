#pragma once

// Extended tools for nxtllm: filesystem reading, ripgrep search,
// and web fetch (via `lightpanda fetch --dump markdown`).
//
// Subprocess-backed tools run blocking work on a std::async thread
// and yield the scheduler periodically so the per-tool spinner card
// in tool_ui.hpp animates while the work is in flight.

#include <nxtai/tools.hpp>
#include <nxtio/async-core.hpp>

#include <nlohmann/json.hpp>

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

inline nlohmann::json object_schema(
    nlohmann::json properties, nlohmann::json required)
{
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

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

inline tools::function_tool make_read_file()
{
    tools::function_tool t;
    t.name = "read_file";
    t.description =
        "Read a text file from the local filesystem. Returns "
        "JSON with the file path, byte count, and the file's "
        "contents truncated to 80 KiB.";
    t.parameters = object_schema(
        {
            {"path",
             {{"type", "string"},
              {"description",
               "Absolute or relative path to the file"}}},
        },
        {"path"});
    t.strict = true;
    t.run = [](const nlohmann::json & args)
        -> nxt::task<std::string> {
        auto path = args.value("path", std::string{});
        if (path.empty())
            co_return nlohmann::json{
                {"error", "missing required parameter `path`"}}
                .dump();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            co_return nlohmann::json{
                {"error", "file does not exist"},
                {"path", path}}
                .dump();
        auto contents = read_file_to_string(path, 80 * 1024);
        co_return nlohmann::json{
            {"path", path},
            {"bytes", contents.size()},
            {"contents", contents},
        }.dump();
    };
    return t;
}

inline tools::function_tool
make_rg_search(nxt::scheduler & sched)
{
    tools::function_tool t;
    t.name = "rg_search";
    t.description =
        "Search for a regex pattern across files using ripgrep. "
        "Returns JSON with the matching lines (file:line:text). "
        "Use this to locate symbols, definitions, or usages.";
    t.parameters = object_schema(
        {
            {"pattern",
             {{"type", "string"},
              {"description",
               "Regex pattern to search for (rg-style)"}}},
            {"path",
             {{"type", "string"},
              {"description",
               "Directory or file to search; use \".\" for "
               "current working directory"}}},
        },
        {"pattern", "path"});
    t.strict = true;
    t.run = [&sched](const nlohmann::json & args)
        -> nxt::task<std::string> {
        auto pattern = args.value("pattern", std::string{});
        auto path = args.value("path", std::string{"."});
        if (pattern.empty())
            co_return nlohmann::json{
                {"error", "missing pattern"}}
                .dump();
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
            sched, std::move(argv), 60 * 1024);
        co_return nlohmann::json{
            {"pattern", pattern},
            {"path", path},
            {"bytes", output.size()},
            {"output", output},
        }.dump();
    };
    return t;
}

inline tools::function_tool
make_web_fetch(nxt::scheduler & sched)
{
    tools::function_tool t;
    t.name = "web_fetch";
    t.description =
        "Fetch a URL and return its content as Markdown using "
        "lightpanda (a headless browser). Useful for reading "
        "documentation pages, blog posts, or any public web "
        "content. Renders JS before extracting.";
    t.parameters = object_schema(
        {
            {"url",
             {{"type", "string"},
              {"description", "HTTPS URL to fetch"}}},
        },
        {"url"});
    t.strict = true;
    t.run = [&sched](const nlohmann::json & args)
        -> nxt::task<std::string> {
        auto url = args.value("url", std::string{});
        if (url.empty())
            co_return nlohmann::json{
                {"error", "missing url"}}
                .dump();
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
            sched, std::move(argv), 120 * 1024);
        co_return nlohmann::json{
            {"url", url},
            {"bytes", output.size()},
            {"output", output},
        }.dump();
    };
    return t;
}

inline tools::function_tool make_bash(nxt::scheduler & sched)
{
    tools::function_tool t;
    t.name = "bash";
    t.description =
        "Run a bash command. The combined stdout+stderr is "
        "returned. This tool REQUIRES user approval — the user "
        "will be prompted to confirm or deny before the command "
        "runs. Use it for read-only inspections and idempotent "
        "operations; avoid destructive commands.";
    t.parameters = object_schema(
        {
            {"command",
             {{"type", "string"},
              {"description",
               "Full shell command line. Will be passed to "
               "/bin/bash -c."}}},
        },
        {"command"});
    t.strict = true;
    t.run = [&sched](const nlohmann::json & args)
        -> nxt::task<std::string> {
        auto cmd = args.value("command", std::string{});
        if (cmd.empty())
            co_return nlohmann::json{
                {"error", "missing command"}}
                .dump();
        std::vector<std::string> argv = {
            "/bin/bash", "-c", cmd};
        auto output = co_await run_subprocess_async(
            sched, std::move(argv), 80 * 1024);
        co_return nlohmann::json{
            {"command", cmd},
            {"bytes", output.size()},
            {"output", output},
        }.dump();
    };
    return t;
}

inline std::vector<tools::function_tool>
for_agent(nxt::scheduler & sched)
{
    std::vector<tools::function_tool> out;
    out.push_back(make_read_file());
    out.push_back(make_rg_search(sched));
    out.push_back(make_web_fetch(sched));
    out.push_back(make_bash(sched));
    return out;
}

} // namespace nxt::ai::agent_tools
