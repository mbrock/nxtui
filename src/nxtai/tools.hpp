#pragma once

#include <nxtai/openai_types.hpp>
#include <nxtio/async.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nxt::ai::tools {

/// Function-call item emitted by a Responses model.
struct function_call
{
    /// Output item id, when present.
    std::string id;
    /// Stable call id used when returning `function_call_output`.
    std::string call_id;
    /// Tool name selected by the model.
    std::string name;
    /// Raw JSON argument string from the response item.
    std::string arguments;
    /// Original response output item.
    openai::raw_json item;
};

/// Local executable function tool plus its JSON schema.
struct function_tool
{
    /// Name exposed to the model.
    std::string name;
    /// Human-readable description exposed to the model.
    std::string description;
    /// JSON schema for arguments.
    nlohmann::json parameters = nlohmann::json::object();
    /// Whether the model must conform strictly to `parameters`.
    bool strict = true;
    /// Coroutine called with parsed arguments to produce tool output text.
    std::function<nxt::task<std::string>(const nlohmann::json &)> run;
};

/// Convert one local function tool to the Responses tool-definition object.
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

/// Convert a list of local function tools to a Responses `tools` array.
[[nodiscard]] inline nlohmann::json
function_tool_definitions(const std::vector<function_tool> & tools)
{
    auto out = nlohmann::json::array();
    for (const auto & tool : tools)
        out.push_back(function_tool_definition(tool));
    return out;
}

/// Parse a Responses output item as a function call when possible.
[[nodiscard]] inline std::optional<function_call>
function_call_from_item(const openai::raw_json & raw_item)
{
    if (raw_item.str.empty())
        return std::nullopt;

    auto item = openai::function_call_item{};
    if (auto ec = glz::read<openai::json_read_opts>(item, raw_item.str))
        throw std::runtime_error{glz::format_error(ec, raw_item.str)};
    if (item.type != "function_call")
        return std::nullopt;

    auto call_id = std::move(item.call_id);
    auto name = std::move(item.name);
    if (call_id.empty() || name.empty())
        return std::nullopt;

    return function_call{
        .id = std::move(item.id),
        .call_id = std::move(call_id),
        .name = std::move(name),
        .arguments = std::move(item.arguments),
        .item = raw_item,
    };
}

/// Parse all function-call output items from a completed response.
[[nodiscard]] inline std::vector<function_call> function_calls_from_items(
    const std::vector<openai::raw_json> & output_items)
{
    auto calls = std::vector<function_call>{};
    for (const auto & item : output_items)
        if (auto call = function_call_from_item(item))
            calls.push_back(std::move(*call));
    return calls;
}

/// Build the structured input item that returns output for a function call.
[[nodiscard]] inline nlohmann::json
function_call_output(std::string call_id, std::string output)
{
    return {
        {"type", "function_call_output"},
        {"call_id", std::move(call_id)},
        {"output", std::move(output)},
    };
}

/// Find and execute a local tool, returning a JSON error string on failure.
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
