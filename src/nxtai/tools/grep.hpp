#pragma once

#include <nxt/baltics.hpp>
#include <nxtai/tools.hpp>
#include <nxtai/tools/subprocess.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::agent_tools {

struct rg_search_tool
{
    static constexpr std::string_view name = "rg_search";
    static constexpr std::string_view description =
        "Search for a regex pattern across files using ripgrep. "
        "Returns JSON with the matching lines (file:line:text). "
        "Use this to locate symbols, definitions, or usages.";
    static constexpr bool strict = true;
    static constexpr std::string_view icon = "grep";

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

    static std::string parameters_summary(const parameters & args)
    {
        auto path = args.path.empty() ? std::string{"."} : args.path;
        return "/" + args.pattern + "/ in " + path;
    }

    static nxt::Rgba8 theme_color(const nxt::theme::Palette & palette)
    {
        return palette.amber;
    }

    nxt::scheduler * sched = nullptr;

    nxt::task<tools::tool_result> run(parameters args) const
    {
        if (args.pattern.empty())
            co_return tools::tool_result{
                .failed = true,
                .output = "missing pattern",
            };

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
        co_return tools::tool_result{
            .output = std::move(output),
        };
    }
};

} // namespace nxt::ai::agent_tools
