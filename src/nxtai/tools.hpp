#pragma once

#include <nxtai/openai_types.hpp>
#include <nxtio/async.hpp>

#include <glaze/glaze_exceptions.hpp>
#include <glaze/json/generic.hpp>
#include <glaze/json/schema.hpp>

#include <concepts>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxt::ai::tools {

template<typename Tool>
concept function_tool = requires(
    const Tool & tool,
    typename Tool::parameters parameters)
{
    { Tool::name } -> std::convertible_to<std::string_view>;
    { Tool::description } -> std::convertible_to<std::string_view>;
    { Tool::strict } -> std::convertible_to<bool>;
    { tool.run(std::move(parameters)) } -> std::same_as<nxt::task<std::string>>;
};

inline constexpr auto tool_schema_opts =
    glz::opts{.error_on_missing_keys = true};

template<typename Parameters>
[[nodiscard]] inline openai::raw_json parameters_schema()
{
    auto schema = glz::ex::read_json<glz::generic>(
        glz::ex::write_json_schema<Parameters, tool_schema_opts>());
    schema.get_object().erase("title");
    if (schema.contains("properties") && !schema.contains("required"))
        schema["required"] = glz::generic::array_t{};
    return openai::raw_json{glz::ex::write_json(schema)};
}

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

/// Heterogeneous compile-time set of concrete function tools.
template<function_tool... Tools>
struct tool_set
{
    std::tuple<Tools...> items;

    explicit tool_set(Tools... tools)
        : items(std::move(tools)...)
    {
    }
};

template<function_tool... Tools>
tool_set(Tools...) -> tool_set<Tools...>;

template<function_tool... Tools>
[[nodiscard]] constexpr bool empty(const tool_set<Tools...> &) noexcept
{
    return sizeof...(Tools) == 0;
}

template<function_tool... Left, function_tool... Right>
[[nodiscard]] auto concat(tool_set<Left...> left, tool_set<Right...> right)
{
    auto joined = std::tuple_cat(std::move(left.items), std::move(right.items));
    return std::apply(
        [](auto &&... tools) {
            return tool_set<std::decay_t<decltype(tools)>...>{
                std::forward<decltype(tools)>(tools)...};
        },
        std::move(joined));
}

/// Convert one concrete function tool to the Responses tool definition object.
template<function_tool Tool>
[[nodiscard]] inline openai::function_tool_definition
function_tool_definition(const Tool &)
{
    using tool_t = std::remove_cvref_t<Tool>;
    return openai::function_tool_definition{
        .name = std::string{tool_t::name},
        .description = std::string{tool_t::description},
        .parameters = parameters_schema<typename tool_t::parameters>(),
        .strict = tool_t::strict,
    };
}

/// Convert a compile-time set of function tools to a Responses `tools` array.
template<function_tool... Tools>
[[nodiscard]] inline std::vector<openai::function_tool_definition>
function_tool_definitions(const tool_set<Tools...> & tools)
{
    auto out = std::vector<openai::function_tool_definition>{};
    out.reserve(sizeof...(Tools));
    std::apply(
        [&](const auto &... tool) {
            (out.emplace_back(function_tool_definition(tool)), ...);
        },
        tools.items);
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

struct function_call_output_item
{
    std::string type = "function_call_output";
    std::string call_id;
    std::string output;
};

/// Build the structured input item that returns output for a function call.
[[nodiscard]] inline openai::raw_json
function_call_output(std::string call_id, std::string output)
{
    auto item = function_call_output_item{
        .call_id = std::move(call_id),
        .output = std::move(output),
    };
    return openai::raw_json{glz::ex::write_json(item)};
}

struct tool_error
{
    std::string error;
    std::string name;
    std::string detail;
    std::string arguments;
};

[[nodiscard]] inline std::string
tool_error_json(
    std::string error,
    std::string name,
    std::string detail = {},
    std::string arguments = {})
{
    return glz::ex::write_json(tool_error{
        .error = std::move(error),
        .name = std::move(name),
        .detail = std::move(detail),
        .arguments = std::move(arguments),
    });
}

template<function_tool Tool>
nxt::task<std::string> run_one_function_tool(
    const Tool & tool,
    const function_call & call)
{
    using parameters_t = typename std::remove_cvref_t<Tool>::parameters;
    auto arguments = parameters_t{};
    if (!call.arguments.empty()) {
        if (auto ec =
                glz::read<openai::json_read_opts>(arguments, call.arguments))
            co_return tool_error_json(
                "invalid tool arguments json",
                call.name,
                glz::format_error(ec, call.arguments),
                call.arguments);
    }

    try {
        co_return co_await tool.run(std::move(arguments));
    } catch (const std::exception & e) {
        co_return tool_error_json("tool execution failed", call.name, e.what());
    } catch (...) {
        co_return tool_error_json(
            "tool execution failed", call.name, "non-std exception");
    }
}

/// Find and execute a concrete tool from a compile-time set.
template<std::size_t I = 0, function_tool... Tools>
nxt::task<std::string> run_function_tool(
    const tool_set<Tools...> & tools,
    const function_call & call)
{
    if constexpr (I == sizeof...(Tools)) {
        co_return tool_error_json("unknown tool", call.name);
    } else {
        const auto & tool = std::get<I>(tools.items);
        using tool_t = std::remove_cvref_t<decltype(tool)>;
        if (call.name == tool_t::name)
            co_return co_await run_one_function_tool(tool, call);
        co_return co_await run_function_tool<I + 1>(tools, call);
    }
}

} // namespace nxt::ai::tools
