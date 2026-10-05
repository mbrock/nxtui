#pragma once

#include <nxt/json.hpp>
#include <nxtrt/pool.hpp>
#if !defined(_WIN32)
#include <nxtrt/scoped_process.hpp>
#endif
#include <nxtrt/task.hpp>
#include <nxtai/openai_types.hpp>
#include <nxtai/tool_json.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/**
 * @namespace nxtai::tools
 * Function tools for the model: defining them, registering them, parsing
 * the model's calls, and running a batch of calls with bounded
 * concurrency.
 *
 * A tool is a type satisfying `function_tool` (static `name`,
 * `description`, `strict`, `parameters_schema_json`, `parse_parameters`,
 * and a `run` coroutine). `make_function_tool` erases it into a
 * `function_tool_entry`; a `tool_registry` is a list of entries. Calls are
 * read from response items with `read_function_call_from_item` and run
 * with `run_function_tool_batch`, or fed to an `nxtrt::pool` as
 * `function_call_idea` recipes. `tool_json.hpp` holds the small JSON
 * readers tools use to parse their arguments. See @ref ai_overview.
 */
namespace nxtai::tools {

/// Outcome of one tool call, as reported back to the model.
struct tool_result
{
    /// True when the call failed; the model sees this flag.
    bool failed = false;
    /// Text returned to the model (output, or an error message).
    std::string output;
#if !defined(_WIN32)
    /// cgroup samples of the tool's process, when it ran in a systemd scope.
    std::optional<nxtrt::scoped_process::observation> observed = {};
#endif
};

/// A statically described function tool.
///
/// Requires static `name`, `description`, and `strict`, a nested
/// `parameters` type, and a const `run(parameters)` returning
/// `nxtrt::task<tool_result>`. `make_function_tool` additionally needs
/// `parameters_schema_json` (see `explicit_tool_schema`) and a static
/// `parse_parameters` (see `explicit_tool_parameter_parser`).
template<typename Tool>
concept function_tool = requires(
    const Tool & tool,
    typename Tool::parameters parameters)
{
    { Tool::name } -> std::convertible_to<std::string_view>;
    { Tool::description } -> std::convertible_to<std::string_view>;
    { Tool::strict } -> std::convertible_to<bool>;
    { tool.run(std::move(parameters)) }
        -> std::same_as<nxtrt::task<tool_result>>;
};

/// Tool provides its JSON Schema as `parameters_schema_json`.
template<typename Tool>
concept explicit_tool_schema = requires
{
    { Tool::parameters_schema_json } -> std::convertible_to<std::string_view>;
};

/// Tool provides `static std::optional<parameters>
/// parse_parameters(std::string_view)`, returning `std::nullopt` for
/// arguments it cannot use.
template<typename Tool>
concept explicit_tool_parameter_parser = requires(std::string_view json)
{
    { Tool::parse_parameters(json) }
        -> std::same_as<std::optional<typename Tool::parameters>>;
};

/// Read one token and report whether it has `kind`.
inline nxtrt::task<bool> take_json_token(
    nxt::json::string_reader & in,
    nxt::json::token_kind kind)
{
    auto token = co_await nxt::json::read_token(in);
    co_return token && token->kind == kind;
}

/// Skip the rest of the value that starts with `first`; false on
/// truncated input. Brackets are counted, not matched.
inline nxtrt::task<bool> skip_json_value(
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

inline nxtrt::task<bool> skip_next_json_value(nxt::json::string_reader & in)
{
    auto token = co_await nxt::json::read_token(in);
    if (!token)
        co_return false;
    co_return co_await skip_json_value(in, std::move(*token));
}

inline nxtrt::task<std::optional<std::string>>
read_json_string_token(nxt::json::string_reader & in)
{
    auto token = co_await nxt::json::read_token(in);
    if (!token || token->kind != nxt::json::token_kind::string)
        co_return std::nullopt;
    co_return std::move(token->text);
}

/// One function call requested by the model.
struct function_call
{
    std::string id = {};
    std::string call_id = {};
    std::string name = {};
    std::string arguments = {};
    openai::raw_json item = {};
};

/// Parse a response output item as a function call.
///
/// Returns `std::nullopt` unless the item is a JSON object with `type`
/// `function_call`, a non-empty `call_id`, and a non-empty `name`.
/// `arguments` is the still-encoded JSON string the model produced. The
/// original item is kept in `item`.
inline nxtrt::task<std::optional<function_call>>
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

/// Type-erased tool: its definition plus a callable taking the raw
/// arguments JSON. Built by `make_function_tool`.
struct function_tool_entry
{
    std::string name;
    std::string description;
    openai::raw_json parameters;
    bool strict = true;
    std::function<nxtrt::task<tool_result>(std::string_view)> run;
};

/// The set of tools offered to the model, looked up by name.
///
/// Running calls borrows the registry: it must stay alive and unchanged
/// until every call (and any pool running them) has settled.
struct tool_registry
{
    /// Tools in definition order. The first entry with a matching name
    /// handles a call.
    std::vector<function_tool_entry> entries;
};

/// True when the registry has no tools.
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

/// Definitions for every registered tool, for `openai_responses_request`.
[[nodiscard]] inline std::vector<openai::function_tool_definition>
function_tool_definitions(const tool_registry & tools)
{
    auto out = std::vector<openai::function_tool_definition>{};
    out.reserve(tools.entries.size());
    for (const auto & tool : tools.entries)
        out.emplace_back(function_tool_definition(tool));
    return out;
}

/// The function calls among `output_items`, in order; other items and
/// malformed calls are skipped.
[[nodiscard]] inline nxtrt::task<std::vector<function_call>>
read_function_calls_from_items(std::vector<openai::raw_json> output_items)
{
    auto calls = std::vector<function_call>{};
    for (auto & item : output_items)
        if (auto call = co_await read_function_call_from_item(std::move(item)))
            calls.push_back(std::move(*call));
    co_return calls;
}

/// `function_call_output` input item answering `call_id` with `output`.
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

/// `{"failed":...,"output":...}`: the output string the model receives for
/// a call.
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

/// Parse `arguments_json` and run `tool`.
///
/// Empty arguments run the tool with default-constructed parameters.
/// Unparseable arguments and exceptions from `run` become failed results;
/// `nxtrt::operation_cancelled` is rethrown.
template<function_tool Tool>
nxtrt::task<tool_result> run_one_function_tool(
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
            };
        arguments = std::move(*parsed);
    }

    try {
        co_return co_await tool.run(std::move(arguments));
    } catch (const nxtrt::operation_cancelled &) {
        throw;
    } catch (const std::exception & e) {
        co_return tool_result{
            .failed = true,
            .output = std::string{"tool execution failed: "} + e.what(),
        };
    } catch (...) {
        co_return tool_result{
            .failed = true,
            .output = "tool execution failed: non-std exception",
        };
    }
}

/// Erase `tool` into a registry entry. The entry owns a copy of the tool.
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

/// Build a registry from entries.
[[nodiscard]] inline tool_registry make_tool_registry(
    std::vector<function_tool_entry> entries)
{
    return tool_registry{.entries = std::move(entries)};
}

/// Run `call` with the first registry entry of the same name, or return a
/// failed `unknown tool` result.
inline nxtrt::task<tool_result> run_function_tool(
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
    };
}

/// A call together with its result and the `function_call_output` item to
/// send back.
struct function_call_result
{
    function_call call;
    tool_result result;
    openai::raw_json output_item;
};

/// Run one call and package the result with its output item.
inline nxtrt::task<function_call_result>
run_function_call(const tool_registry & tools, function_call call)
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

/// Pool recipe (idea) for one call: owns the call, borrows the registry.
///
/// An `nxtrt::pool<function_call_idea>` runs these and publishes
/// `function_call_result`s in completion order. Tool errors are failed
/// results; cancellation and uncaught infrastructure errors throw. The
/// registry must outlive the pool's drain. Invoking it moves the call out,
/// so each idea runs once.
struct function_call_idea
{
    const tool_registry * tools;
    function_call call;

    nxtrt::task<function_call_result> operator()() &
    {
        return run_function_call(*tools, std::move(call));
    }
};

/// Run `calls` with at most `max_in_flight` running at once and return
/// their results in input order.
///
/// Calls may finish in any order; each result is written to its input
/// position. Tool failures (unknown tool, bad arguments, exceptions from a
/// typed tool) are failed results and do not affect other calls. Any other
/// exception is held until every call has settled, then the first one in
/// input order is rethrown. Cancellation stops admitting calls, cancels
/// running ones, and waits for them before propagating.
///
/// `tools` is borrowed until the returned task completes. Admission is
/// bounded, but all calls and results are kept until the end.
/// @throws std::invalid_argument if `max_in_flight` is zero.
inline nxtrt::task<std::vector<function_call_result>>
run_function_tool_batch(
    const tool_registry & tools,
    std::vector<function_call> calls,
    std::size_t max_in_flight = 4)
{
    if (max_in_flight == 0)
        throw std::invalid_argument{"tool batch needs nonzero capacity"};
    nxtrt::throw_if_stop_requested();
    if (calls.empty())
        co_return std::vector<function_call_result>{};

    using outcome = std::expected<function_call_result, std::exception_ptr>;

    struct batch_call
    {
        function_call_idea work;
        std::optional<outcome> * result;

        nxtrt::task<void> operator()() &
        {
            try {
                result->emplace(co_await work());
            } catch (const nxtrt::operation_cancelled &) {
                throw;
            } catch (...) {
                result->emplace(std::unexpected{std::current_exception()});
            }
        }
    };

    auto outcomes = std::vector<std::optional<outcome>>(calls.size());
    auto recipes = std::views::iota(std::size_t{0}, calls.size())
                   | std::views::transform([&](std::size_t i) {
                         return batch_call{
                             {&tools, std::move(calls[i])}, &outcomes[i]};
                     });
    auto input_land = nxtrt::static_value_storage<batch_call, 1>{};
    auto input = nxtrt::value_range_source{recipes, input_land.ref()};
    auto capacity = std::min(max_in_flight, calls.size());
    co_await nxtrt::drain(input, capacity);

    auto out = std::vector<function_call_result>{};
    out.reserve(outcomes.size());
    for (auto & result : outcomes) {
        if (!*result)
            nxtrt::rethrow(result->error());
        out.push_back(std::move(**result));
    }
    co_return out;
}

/// Copy the `output_item` of each result, in order.
[[nodiscard]] inline std::vector<openai::raw_json> output_items_from_results(
    std::vector<function_call_result> & results)
{
    auto out = std::vector<openai::raw_json>{};
    out.reserve(results.size());
    for (auto & result : results)
        out.push_back(result.output_item);
    return out;
}

} // namespace nxtai::tools
