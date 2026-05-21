#pragma once

#include <nxt/baltics.hpp>
#include <nxtai/tools.hpp>
#include <nxtai/tools/subprocess.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::agent_tools {

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
    static constexpr std::string_view icon = "bash";

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

    static std::string parameters_summary(const parameters & args)
    {
        return args.command;
    }

    static nxt::Rgba8 theme_color(const nxt::theme::Palette & palette)
    {
        return palette.doc_orange;
    }

    nxt::scheduler * sched = nullptr;

    nxt::task<tools::tool_result> run(parameters args) const
    {
        if (args.command.empty())
            co_return tools::tool_result{
                .failed = true,
                .output = "missing command",
            };

        auto command = args.command;
        std::vector<std::string> argv = {"/bin/bash", "-c", command};
        auto output = co_await run_subprocess_async(
            *sched, std::move(argv), 80 * 1024);
        co_return tools::tool_result{
            .output = std::move(output),
        };
    }
};

} // namespace nxt::ai::agent_tools
