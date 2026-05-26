#pragma once

#include <nxt/json.hpp>
#include <nxt/rt/scoped_process.hpp>
#include <nxt/rt/task.hpp>
#include <nxtai/openai_types.hpp>
#include <nxtai/tool_json.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxt::ai::tools {

struct tool_result
{
    bool failed = false;
    std::string output;
    std::optional<nxt::rt::scoped_process::observation> observed;
};

template<typename Tool>
concept function_tool = requires(
    const Tool & tool,
    typename Tool::parameters parameters)
{
    { Tool::name } -> std::convertible_to<std::string_view>;
    { Tool::description } -> std::convertible_to<std::string_view>;
    { Tool::strict } -> std::convertible_to<bool>;
    { tool.run(std::move(parameters)) }
        -> std::same_as<nxt::rt::task<tool_result>>;
};

template<typename Tool>
concept explicit_tool_schema = requires
{
    { Tool::parameters_schema_json } -> std::convertible_to<std::string_view>;
};

template<typename Tool>
concept explicit_tool_parameter_parser = requires(std::string_view json)
{
    { Tool::parse_parameters(json) }
        -> std::same_as<std::optional<typename Tool::parameters>>;
};

inline nxt::rt::task<bool> take_json_token(
    nxt::json::string_reader & in,
    nxt::json::token_kind kind)
{
    auto token = co_await nxt::json::read_token(in);
    co_return token && token->kind == kind;
}

inline nxt::rt::task<bool> skip_json_value(
    nxt::json::string_reader & in,
    nxt::json::token first)
{
    auto depth = 0;
    if (first.kind == nxt::json::token_kind::object_begin
        || first.kind == nxt::json::token_kind::array_begin) {
        depth = 1;
    } else {
        co_return true;
    }

    while (depth > 0) {
        auto token = co_await nxt::json::read_token(in);
        if (!token)
            co_return false;
        if (token->kind == nxt::json::token_kind::object_begin
            || token->kind == nxt::json::token_kind::array_begin)
            ++depth;
        else if (
            token->kind == nxt::json::token_kind::object_end
            || token->kind == nxt::json::token_kind::array_end)
            --depth;
    }
    co_return true;
}

inline nxt::rt::task<bool> skip_next_json_value(nxt::json::string_reader & in)
{
    auto token = co_await nxt::json::read_token(in);
    if (!token)
        co_return false;
    co_return co_await skip_json_value(in, std::move(*token));
}

inline nxt::rt::task<std::optional<std::string>>
read_json_string_token(nxt::json::string_reader & in)
{
    auto token = co_await nxt::json::read_token(in);
    if (!token || token->kind != nxt::json::token_kind::string)
        co_return std::nullopt;
    co_return std::move(token->text);
}

struct function_call
{
    std::string id = {};
    std::string call_id = {};
    std::string name = {};
    std::string arguments = {};
    openai::raw_json item = {};
};

inline nxt::rt::task<std::optional<function_call>>
read_function_call_from_item(openai::raw_json raw_item)
{
    if (raw_item.str.empty())
        co_return std::nullopt;

    auto in = nxt::json::string_reader{.input = raw_item.str};
    if (!(co_await take_json_token(in, nxt::json::token_kind::object_begin)))
        co_return std::nullopt;

    auto out = function_call{.item = raw_item};
    auto type = std::string{};
    while (true) {
        auto token = co_await nxt::json::read_token(in);
        if (!token)
            co_return std::nullopt;
        if (token->kind == nxt::json::token_kind::object_end)
            break;
        if (token->kind != nxt::json::token_kind::string)
            co_return std::nullopt;

        auto key = std::move(token->text);
        if (!(co_await take_json_token(in, nxt::json::token_kind::colon)))
            co_return std::nullopt;

        if (key == "id") {
            out.id = (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "type") {
            type = (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "call_id") {
            out.call_id =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "name") {
            out.name =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "arguments") {
            out.arguments =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (!(co_await skip_next_json_value(in))) {
            co_return std::nullopt;
        }

        token = co_await nxt::json::read_token(in);
        if (!token)
            co_return std::nullopt;
        if (token->kind == nxt::json::token_kind::object_end)
            break;
        if (token->kind != nxt::json::token_kind::comma)
            co_return std::nullopt;
    }

    if (type != "function_call" || out.call_id.empty() || out.name.empty())
        co_return std::nullopt;
    co_return out;
}

struct function_tool_entry
{
    std::string name;
    std::string description;
    openai::raw_json parameters;
    bool strict = true;
    std::function<nxt::rt::task<tool_result>(std::string_view)> run;
};

struct tool_registry
{
    std::vector<function_tool_entry> entries;
};

[[nodiscard]] inline bool empty(const tool_registry & tools) noexcept
{
    return tools.entries.empty();
}

template<function_tool Tool>
[[nodiscard]] inline openai::raw_json tool_parameters_schema()
{
    using tool_t = std::remove_cvref_t<Tool>;
    if constexpr (explicit_tool_schema<tool_t>) {
        return openai::raw_json{std::string{tool_t::parameters_schema_json}};
    } else {
        static_assert(
            explicit_tool_schema<tool_t>,
            "tool needs parameters_schema_json");
    }
}

[[nodiscard]] inline openai::function_tool_definition
function_tool_definition(const function_tool_entry & tool)
{
    return openai::function_tool_definition{
        .name = std::string{tool.name},
        .description = std::string{tool.description},
        .parameters = openai::raw_json{tool.parameters.str},
        .strict = tool.strict,
    };
}

[[nodiscard]] inline std::vector<openai::function_tool_definition>
function_tool_definitions(const tool_registry & tools)
{
    auto out = std::vector<openai::function_tool_definition>{};
    out.reserve(tools.entries.size());
    for (const auto & tool : tools.entries)
        out.emplace_back(function_tool_definition(tool));
    return out;
}

[[nodiscard]] inline nxt::rt::task<std::vector<function_call>>
read_function_calls_from_items(std::vector<openai::raw_json> output_items)
{
    auto calls = std::vector<function_call>{};
    for (auto & item : output_items)
        if (auto call = co_await read_function_call_from_item(std::move(item)))
            calls.push_back(std::move(*call));
    co_return calls;
}

[[nodiscard]] inline openai::raw_json
function_call_output(std::string call_id, std::string output)
{
    auto json = nxt::json::writer{};
    json.character('{');
    json.key("type");
    json.string("function_call_output");
    json.character(',');
    json.key("call_id");
    json.string(call_id);
    json.character(',');
    json.key("output");
    json.string(output);
    json.character('}');
    return openai::raw_json{std::move(json.out)};
}

[[nodiscard]] inline std::string tool_result_json(const tool_result & result)
{
    auto json = nxt::json::writer{};
    json.character('{');
    json.key("failed");
    json.boolean(result.failed);
    json.character(',');
    json.key("output");
    json.string(result.output);
    json.character('}');
    return std::move(json.out);
}

template<function_tool Tool>
nxt::rt::task<tool_result> run_one_function_tool(
    const Tool & tool,
    std::string_view arguments_json)
{
    using tool_t = std::remove_cvref_t<Tool>;
    static_assert(
        explicit_tool_parameter_parser<tool_t>,
        "tool needs parse_parameters(std::string_view)");

    auto arguments = typename std::remove_cvref_t<Tool>::parameters{};
    if (!arguments_json.empty()) {
        auto parsed = tool_t::parse_parameters(arguments_json);
        if (!parsed)
            co_return tool_result{
                .failed = true,
                .output = "invalid tool arguments json",
                .observed = std::nullopt,
            };
        arguments = std::move(*parsed);
    }

    try {
        co_return co_await tool.run(std::move(arguments));
    } catch (const std::exception & e) {
        co_return tool_result{
            .failed = true,
            .output = std::string{"tool execution failed: "} + e.what(),
            .observed = std::nullopt,
        };
    } catch (...) {
        co_return tool_result{
            .failed = true,
            .output = "tool execution failed: non-std exception",
            .observed = std::nullopt,
        };
    }
}

template<function_tool Tool>
[[nodiscard]] inline function_tool_entry make_function_tool(Tool tool)
{
    using tool_t = std::remove_cvref_t<Tool>;
    return function_tool_entry{
        .name = std::string{tool_t::name},
        .description = std::string{tool_t::description},
        .parameters = tool_parameters_schema<tool_t>(),
        .strict = tool_t::strict,
        .run =
            [tool = std::move(tool)](std::string_view arguments) {
                return run_one_function_tool(tool, arguments);
            },
    };
}

[[nodiscard]] inline tool_registry make_tool_registry(
    std::vector<function_tool_entry> entries)
{
    return tool_registry{.entries = std::move(entries)};
}

inline nxt::rt::task<tool_result> run_function_tool(
    const tool_registry & tools,
    const function_call & call)
{
    for (const auto & tool : tools.entries) {
        if (call.name == tool.name)
            co_return co_await tool.run(call.arguments);
    }
    co_return tool_result{
        .failed = true,
        .output = "unknown tool",
        .observed = std::nullopt,
    };
}

struct function_call_result
{
    function_call call;
    tool_result result;
    openai::raw_json output_item;
};

inline nxt::rt::task<function_call_result> run_one_call_for_batch(
    const tool_registry & tools,
    function_call call)
{
    auto result = co_await run_function_tool(tools, call);
    auto output_item =
        function_call_output(call.call_id, tool_result_json(result));
    co_return function_call_result{
        .call = std::move(call),
        .result = std::move(result),
        .output_item = std::move(output_item),
    };
}

inline nxt::rt::task<std::vector<function_call_result>> run_function_tool_batch(
    const tool_registry & tools,
    std::vector<function_call> calls)
{
    auto deeds = co_await nxt::rt::with_zone(
        [&]() -> nxt::rt::task<
            std::vector<nxt::rt::catching_deed<function_call_result>>> {
            auto out =
                std::vector<nxt::rt::catching_deed<function_call_result>>{};
            out.reserve(calls.size());
            for (auto & call : calls)
                out.push_back(
                    nxt::rt::fork(
                        run_one_call_for_batch(tools, std::move(call)))
                        .cope());
            co_return out;
        });

    auto out = std::vector<function_call_result>{};
    out.reserve(deeds.size());
    for (auto & deed : deeds) {
        auto result = std::move(deed).get();
        if (result) {
            out.push_back(std::move(*result));
        } else {
            nxt::rt::rethrow(result.error());
        }
    }
    co_return out;
}

[[nodiscard]] inline std::vector<openai::raw_json> output_items_from_results(
    std::vector<function_call_result> & results)
{
    auto out = std::vector<openai::raw_json>{};
    out.reserve(results.size());
    for (auto & result : results)
        out.push_back(result.output_item);
    return out;
}

} // namespace nxt::ai::tools
