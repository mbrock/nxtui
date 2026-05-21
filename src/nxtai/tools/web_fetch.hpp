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

struct web_fetch_tool
{
    static constexpr std::string_view name = "web_fetch";
    static constexpr std::string_view description =
        "Fetch a URL and return its content as Markdown using "
        "lightpanda (a headless browser). Useful for reading "
        "documentation pages, blog posts, or any public web "
        "content. Renders JS before extracting.";
    static constexpr bool strict = true;
    static constexpr std::string_view icon = "look";

    struct parameters
    {
        std::string url;

        struct glaze_json_schema
        {
            glz::schema url{.description = "HTTPS URL to fetch"};
        };
    };

    static std::string parameters_summary(const parameters & args)
    {
        return args.url;
    }

    static nxt::Rgba8 theme_color(const nxt::theme::Palette & palette)
    {
        return palette.pink;
    }

    nxt::scheduler * sched = nullptr;

    nxt::task<tools::tool_result> run(parameters args) const
    {
        if (args.url.empty())
            co_return tools::tool_result{
                .failed = true,
                .output = "missing url",
            };

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
        co_return tools::tool_result{
            .output = std::move(output),
        };
    }
};

} // namespace nxt::ai::agent_tools
