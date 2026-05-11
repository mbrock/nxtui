#pragma once

#include <nxtio/async.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace nxt::ai::tools {

struct function_call
{
    std::string id;
    std::string call_id;
    std::string name;
    std::string arguments;
    nlohmann::json item = nlohmann::json::object();
};

struct function_tool
{
    std::string name;
    std::string description;
    nlohmann::json parameters = nlohmann::json::object();
    bool strict = true;
    std::function<nxt::task<std::string>(const nlohmann::json &)> run;
};

[[nodiscard]] inline nlohmann::json
function_tool_definition(const function_tool & tool)
{
    auto out = nlohmann::json{
        {"type", "function"},
        {"name", tool.name},
        {"description", tool.description},
        {"parameters", tool.parameters},
        {"strict", tool.strict},
    };
    return out;
}

[[nodiscard]] inline nlohmann::json
function_tool_definitions(const std::vector<function_tool> & tools)
{
    auto out = nlohmann::json::array();
    for (const auto & tool : tools)
        out.push_back(function_tool_definition(tool));
    return out;
}

[[nodiscard]] inline std::optional<function_call>
function_call_from_item(const nlohmann::json & item)
{
    if (!item.is_object() || item.value("type", std::string{}) != "function_call")
        return std::nullopt;

    auto call_id = item.value("call_id", std::string{});
    auto name = item.value("name", std::string{});
    if (call_id.empty() || name.empty())
        return std::nullopt;

    return function_call{
        .id = item.value("id", std::string{}),
        .call_id = std::move(call_id),
        .name = std::move(name),
        .arguments = item.value("arguments", std::string{}),
        .item = item,
    };
}

[[nodiscard]] inline nlohmann::json
function_call_output(std::string call_id, std::string output)
{
    return {
        {"type", "function_call_output"},
        {"call_id", std::move(call_id)},
        {"output", std::move(output)},
    };
}

inline nxt::task<std::string> run_function_tool(
    const std::vector<function_tool> & tools,
    const function_call & call)
{
    auto it = std::ranges::find_if(tools, [&](const function_tool & tool) {
        return tool.name == call.name;
    });
    if (it == tools.end()) {
        co_return nlohmann::json{
            {"error", "unknown tool"},
            {"name", call.name},
        }.dump();
    }
    if (!it->run) {
        co_return nlohmann::json{
            {"error", "tool has no executor"},
            {"name", call.name},
        }.dump();
    }

    nlohmann::json arguments = nlohmann::json::object();
    if (!call.arguments.empty()) {
        try {
            arguments = nlohmann::json::parse(call.arguments);
        } catch (const nlohmann::json::exception & e) {
            co_return nlohmann::json{
                {"error", "invalid tool arguments json"},
                {"name", call.name},
                {"detail", e.what()},
                {"arguments", call.arguments},
            }.dump();
        }
    }

    try {
        co_return co_await it->run(arguments);
    } catch (const std::exception & e) {
        co_return nlohmann::json{
            {"error", "tool execution failed"},
            {"name", call.name},
            {"detail", e.what()},
        }.dump();
    }
}

} // namespace nxt::ai::tools
