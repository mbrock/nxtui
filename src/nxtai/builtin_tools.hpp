#pragma once

#include <nxtai/tools.hpp>
#include <nxtio/app.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nxt::ai::builtin_tools {

inline nlohmann::json
object_schema(nlohmann::json properties, nlohmann::json required)
{
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

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

inline std::vector<tools::function_tool>
for_runtime(nxt::ui::UIRuntime & runtime)
{
    auto out = std::vector<tools::function_tool>{};

    out.push_back(tools::function_tool{
        .name = "nxt_current_time",
        .description =
            "Return the current local timestamp for the nxtllm process.",
        .parameters = object_schema(
            nlohmann::json::object(),
            nlohmann::json::array()),
        .strict = true,
        .run = [](const nlohmann::json &) -> nxt::task<std::string> {
            co_return nlohmann::json{
                {"local_time", local_timestamp()},
            }.dump();
        },
    });

    out.push_back(tools::function_tool{
        .name = "nxt_terminal_size",
        .description =
            "Return the current terminal size used by the nxt UI runtime.",
        .parameters = object_schema(
            nlohmann::json::object(),
            nlohmann::json::array()),
        .strict = true,
        .run = [&runtime](const nlohmann::json &) -> nxt::task<std::string> {
            co_return nlohmann::json{
                {"columns", runtime.terminal_width().count()},
                {"rows", runtime.terminal_height().count()},
            }.dump();
        },
    });

    out.push_back(tools::function_tool{
        .name = "nxt_echo",
        .description =
            "Echo a short text string. Useful for checking that tool calling works.",
        .parameters = object_schema(
            {
                {"text",
                 {
                     {"type", "string"},
                     {"description", "Text to echo back."},
                 }},
            },
            {"text"}),
        .strict = true,
        .run = [](const nlohmann::json & args) -> nxt::task<std::string> {
            co_return nlohmann::json{
                {"text", args.value("text", std::string{})},
            }.dump();
        },
    });

    return out;
}

} // namespace nxt::ai::builtin_tools
