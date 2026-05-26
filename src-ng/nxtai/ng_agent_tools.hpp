#pragma once

#include <nxtai/tool_batch.hpp>
#include <nxtai/tool_process.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace nxt::ai::agent_tools {

inline std::string read_file_to_string(
    const std::filesystem::path & p,
    std::size_t max_bytes)
{
    auto file = std::ifstream{p, std::ios::binary};
    if (!file.is_open())
        return {};
    auto out = std::string{};
    out.resize(max_bytes);
    file.read(out.data(), static_cast<std::streamsize>(max_bytes));
    out.resize(static_cast<std::size_t>(file.gcount()));
    return out;
}

inline tools::tool_result process_result_to_tool_result(
    tool_process::result captured)
{
    auto observed = std::optional<nxt::rt::scoped_process::observation>{};
    if (captured.observed.active())
        observed = std::move(captured.observed);

    if (captured.failed)
        return tools::tool_result{
            .failed = true,
            .output = std::move(captured.output),
            .observed = std::move(observed),
        };

    if (!captured.status.exited || captured.status.exit_code != 0)
        return tools::tool_result{
            .failed = true,
            .output = captured.output.empty()
                ? std::string{"process failed"}
                : std::move(captured.output),
            .observed = std::move(observed),
        };

    return tools::tool_result{
        .output = std::move(captured.output),
        .observed = std::move(observed),
    };
}

struct bash_tool
{
    static constexpr std::string_view name = "bash";
    static constexpr std::string_view description =
        "Run a bash command. The combined stdout+stderr is returned. Use it "
        "for read-only inspections and idempotent operations; avoid "
        "destructive commands.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string command;
    };

    static constexpr std::string_view parameters_schema_json =
        R"json({"type":"object","properties":{"command":{"type":"string","description":"Full shell command line. Will be passed to /bin/bash -c."}},"additionalProperties":false,"required":["command"]})json";

    static std::optional<parameters> parse_parameters(std::string_view json)
    {
        auto command = tools::json_string_member(json, "command");
        if (!command)
            return std::nullopt;
        return parameters{.command = std::move(*command)};
    }

    nxt::rt::task<tools::tool_result> run(parameters args) const
    {
        if (args.command.empty())
            co_return tools::tool_result{
                .failed = true,
                .output = "missing command",
                .observed = std::nullopt,
            };

        auto argv = std::vector<std::string>{};
        argv.emplace_back("/bin/bash");
        argv.emplace_back("-c");
        argv.push_back(std::move(args.command));
        auto captured = co_await tool_process::capture(
            std::move(argv),
            tool_process::capture_options{
                .scope = nxt::rt::scoped_process::options{
                    .systemd_user_scope = true,
                    .unit_name = nxt::rt::scoped_process::make_unit_name(
                        "bash"),
                },
            });
        co_return process_result_to_tool_result(std::move(captured));
    }
};

struct rg_search_tool
{
    static constexpr std::string_view name = "rg_search";
    static constexpr std::string_view description =
        "Search for a regex pattern across files using ripgrep. Returns "
        "matching lines as file:line:text.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string pattern;
        std::string path = ".";
    };

    static constexpr std::string_view parameters_schema_json =
        R"json({"type":"object","properties":{"pattern":{"type":"string","description":"Regex pattern to search for (rg-style)"},"path":{"type":"string","description":"Directory or file to search; use \".\" for the current directory"}},"additionalProperties":false,"required":["pattern","path"]})json";

    static std::optional<parameters> parse_parameters(std::string_view json)
    {
        auto pattern = tools::json_string_member(json, "pattern");
        if (!pattern)
            return std::nullopt;
        auto path = tools::json_string_member(json, "path").value_or(".");
        return parameters{.pattern = std::move(*pattern), .path = std::move(path)};
    }

    nxt::rt::task<tools::tool_result> run(parameters args) const
    {
        if (args.pattern.empty())
            co_return tools::tool_result{
                .failed = true,
                .output = "missing pattern",
                .observed = std::nullopt,
            };

        if (args.path.empty())
            args.path = ".";

        auto argv = std::vector<std::string>{};
        argv.emplace_back("rg");
        argv.emplace_back("--no-heading");
        argv.emplace_back("--line-number");
        argv.emplace_back("--max-count");
        argv.emplace_back("50");
        argv.emplace_back("--max-columns");
        argv.emplace_back("200");
        argv.emplace_back("--");
        argv.push_back(std::move(args.pattern));
        argv.push_back(std::move(args.path));
        auto captured = co_await tool_process::capture(std::move(argv));
        co_return process_result_to_tool_result(std::move(captured));
    }
};

struct read_file_tool
{
    static constexpr std::string_view name = "read_file";
    static constexpr std::string_view description =
        "Read a text file from the local filesystem. Returns up to 8 MiB.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string path;
    };

    static constexpr std::string_view parameters_schema_json =
        R"json({"type":"object","properties":{"path":{"type":"string","description":"Absolute or relative path to the file"}},"additionalProperties":false,"required":["path"]})json";

    static std::optional<parameters> parse_parameters(std::string_view json)
    {
        auto path = tools::json_string_member(json, "path");
        if (!path)
            return std::nullopt;
        return parameters{.path = std::move(*path)};
    }

    nxt::rt::task<tools::tool_result> run(parameters args) const
    {
        auto path = std::move(args.path);
        if (path.empty())
            co_return tools::tool_result{
                .failed = true,
                .output = "missing required parameter `path`",
                .observed = std::nullopt,
            };

        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            co_return tools::tool_result{
                .failed = true,
                .output = "file does not exist",
                .observed = std::nullopt,
            };

        co_return tools::tool_result{
            .output = read_file_to_string(path, 8 * 1024 * 1024),
            .observed = std::nullopt,
        };
    }
};

[[nodiscard]] inline auto for_agent()
{
    return tools::make_tool_registry({
        tools::make_function_tool(read_file_tool{}),
        tools::make_function_tool(rg_search_tool{}),
        tools::make_function_tool(bash_tool{}),
    });
}

} // namespace nxt::ai::agent_tools
