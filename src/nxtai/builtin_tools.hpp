#pragma once

#include <nxtai/tools.hpp>
#include <nxtio/app.hpp>

#include <chrono>
#include <ctime>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace nxt::ai::builtin_tools {

inline std::string local_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = std::tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    auto out = std::ostringstream{};
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S %z");
    return out.str();
}

struct current_time_tool
{
    static constexpr std::string_view name = "nxt_current_time";
    static constexpr std::string_view description =
        "Return the current local timestamp for the nxtllm process.";
    static constexpr bool strict = true;

    struct parameters
    {
    };

    struct result
    {
        std::string local_time;
    };

    nxt::task<std::string> run(parameters) const
    {
        co_return glz::ex::write_json(result{.local_time = local_timestamp()});
    }
};

struct terminal_size_tool
{
    static constexpr std::string_view name = "nxt_terminal_size";
    static constexpr std::string_view description =
        "Return the current terminal size used by the nxt UI runtime.";
    static constexpr bool strict = true;

    struct parameters
    {
    };

    struct result
    {
        std::size_t columns = 0;
        std::size_t rows = 0;
    };

    nxt::ui::UIRuntime * runtime = nullptr;

    nxt::task<std::string> run(parameters) const
    {
        co_return glz::ex::write_json(result{
            .columns = runtime->terminal_width().count(),
            .rows = runtime->terminal_height().count(),
        });
    }
};

struct echo_tool
{
    static constexpr std::string_view name = "nxt_echo";
    static constexpr std::string_view description =
        "Echo a short text string. Useful for checking that tool calling works.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string text;

        struct glaze_json_schema
        {
            glz::schema text{.description = "Text to echo back."};
        };
    };

    struct result
    {
        std::string text;
    };

    static std::string parameters_summary(const parameters & args)
    {
        return args.text;
    }

    nxt::task<std::string> run(parameters args) const
    {
        co_return glz::ex::write_json(result{.text = std::move(args.text)});
    }
};

inline auto
for_runtime(nxt::ui::UIRuntime & runtime)
{
    return tools::tool_set{
        current_time_tool{},
        terminal_size_tool{.runtime = &runtime},
        echo_tool{},
    };
}

} // namespace nxt::ai::builtin_tools
