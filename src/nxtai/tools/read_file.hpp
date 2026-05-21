#pragma once

#include <nxt/baltics.hpp>
#include <nxtai/tools.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

struct read_file_tool
{
    static constexpr std::string_view name = "read_file";
    static constexpr std::string_view description =
        "Read a text file from the local filesystem. Returns "
        "the file contents truncated to 80 KiB.";
    static constexpr bool strict = true;
    static constexpr std::string_view icon = "read";

    struct parameters
    {
        std::string path;

        struct glaze_json_schema
        {
            glz::schema path{
                .description = "Absolute or relative path to the file"};
        };
    };

    static std::string parameters_summary(const parameters & args)
    {
        return args.path;
    }

    static nxt::Rgba8 theme_color(const nxt::theme::Palette & palette)
    {
        return palette.cyan;
    }

    nxt::task<tools::tool_result> run(parameters args) const
    {
        auto path = std::move(args.path);
        if (path.empty())
            co_return tools::tool_result{
                .failed = true,
                .output = "missing required parameter `path`",
            };

        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            co_return tools::tool_result{
                .failed = true,
                .output = "file does not exist",
            };

        auto contents = read_file_to_string(path, 80 * 1024);
        co_return tools::tool_result{
            .output = std::move(contents),
        };
    }
};

} // namespace nxt::ai::agent_tools
