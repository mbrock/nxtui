#pragma once

#include <nxt/rt/scoped_process.hpp>
#include <nxt/rt/task.hpp>
#include <nxtai/openai_types.hpp>

#include <glaze/glaze_exceptions.hpp>
#include <glaze/json/generic.hpp>
#include <glaze/json/schema.hpp>

#include <concepts>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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

struct function_call
{
    std::string id = {};
    std::string call_id = {};
    std::string name = {};
    std::string arguments = {};
    openai::raw_json item = {};
};

template<function_tool... Tools>
struct tool_set
{
    std::tuple<Tools...> items;

    explicit tool_set(Tools... tools)
        : items(std::move(tools)...)
    {}
};

template<function_tool... Tools>
tool_set(Tools...) -> tool_set<Tools...>;

template<function_tool... Tools>
[[nodiscard]] constexpr bool empty(const tool_set<Tools...> &) noexcept
{
    return sizeof...(Tools) == 0;
}

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

[[nodiscard]] inline openai::raw_json
function_call_output(std::string call_id, std::string output)
{
    auto item = function_call_output_item{
        .call_id = std::move(call_id),
        .output = std::move(output),
    };
    return openai::raw_json{glz::ex::write_json(item)};
}

[[nodiscard]] inline std::string tool_result_json(const tool_result & result)
{
    struct serialized_tool_result
    {
        bool failed = false;
        std::string output;
    };

    return glz::ex::write_json(
        serialized_tool_result{
            .failed = result.failed,
            .output = result.output,
        });
}

template<function_tool Tool>
nxt::rt::task<tool_result> run_one_function_tool(
    const Tool & tool,
    const function_call & call)
{
    using parameters_t = typename std::remove_cvref_t<Tool>::parameters;
    auto arguments = parameters_t{};
    if (!call.arguments.empty()) {
        if (auto ec =
                glz::read<openai::json_read_opts>(arguments, call.arguments))
            co_return tool_result{
                .failed = true,
                .output = std::string{"invalid tool arguments json: "}
                    + glz::format_error(ec, call.arguments),
                .observed = std::nullopt,
            };
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

template<std::size_t I = 0, function_tool... Tools>
nxt::rt::task<tool_result> run_function_tool(
    const tool_set<Tools...> & tools,
    const function_call & call)
{
    if constexpr (I == sizeof...(Tools)) {
        co_return tool_result{
            .failed = true,
            .output = "unknown tool",
            .observed = std::nullopt,
        };
    } else {
        const auto & tool = std::get<I>(tools.items);
        using tool_t = std::remove_cvref_t<decltype(tool)>;
        if (call.name == tool_t::name)
            co_return co_await run_one_function_tool(tool, call);
        co_return co_await run_function_tool<I + 1>(tools, call);
    }
}

struct function_call_result
{
    function_call call;
    tool_result result;
    openai::raw_json output_item;
};

template<function_tool... Tools>
nxt::rt::task<function_call_result> run_one_call_for_batch(
    const tool_set<Tools...> & tools,
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

template<function_tool... Tools>
nxt::rt::task<std::vector<function_call_result>> run_function_tool_batch(
    const tool_set<Tools...> & tools,
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
