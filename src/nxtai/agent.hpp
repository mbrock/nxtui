#pragma once

#include <nxtai/responses_request.hpp>
#include <nxtai/responses_stream.hpp>
#include <nxtai/tool_batch.hpp>

#include <algorithm>

/**
 * @namespace nxtai
 * OpenAI Responses client pieces and a tool-calling agent loop, running as
 * ordinary `nxtrt` tasks on a deck.
 *
 * `nxtai::responses` builds the JSON request body
 * (`openai_responses_request`, `openai_responses_body`) and decodes
 * streamed events into a completed response (`stream_decoder`).
 * `nxtai::tools` defines function tools, the `tool_registry`, and the
 * bounded batch runner that executes a response's tool calls.
 * `run_agent` ties them together: request, run tools, send results, repeat.
 * The transport itself (TLS connection, HTTP, SSE parsing over
 * `nxtrt::tls` and `nxtrt::http`) is `responses_transport.hpp`, shared by
 * `nxtllm` and native application hosts. `nxtai::agent_tools` supplies the
 * `read_file`, `rg_search`, and `bash` tools that `nxtllm` uses.
 *
 * See @ref ai_overview for the design, ownership, and current limits.
 */
namespace nxtai {

/// Limits for `run_agent`. Both must be nonzero.
struct agent_options
{
    /// Maximum number of model requests in one `run_agent` call.
    std::size_t max_turns = 32;
    /// Maximum number of tool calls running at once within a turn.
    std::size_t tool_concurrency = 1;
};

/// Run the request/tool loop until the model answers without calling tools.
///
/// Each turn awaits `transport(request, observer)`, which must return an
/// awaitable of `responses::response_result` holding the completed
/// response's output items in order; the transport is also where streamed
/// text reaches the observer (`nxtllm` calls `observer.text(delta)`). The
/// loop then collects the `function_call` items. If there are none, it
/// returns. Otherwise it awaits `observer.tool_started(call)` for each call,
/// runs them with `tools::run_function_tool_batch` (at most
/// `options.tool_concurrency` at once), awaits
/// `observer.tool_finished(result)` for each result in call order, and
/// sends the `function_call_output` items in the next request.
///
/// History: with HTTP and `request.store == false`, every request carries the full
/// transcript (the initial input, every output item as received, including
/// opaque reasoning items, and every tool output). With `store == true`,
/// the next request sets `previous_response_id` and carries only the new
/// tool outputs. A transport advertising `supports_unstored_continuation`
/// (Responses WebSocket) uses that incremental path even with store=false.
///
/// Before the first turn, missing registry definitions are appended to
/// `request.tools` by name, preserving caller definitions. When `store` is
/// false and the registry is not empty, `reasoning.encrypted_content` is
/// appended to `request.include` if absent, preserving other includes.
///
/// `request` is owned by the task. `registry`, `transport`, and `observer`
/// are borrowed and must outlive it.
///
/// Errors: throws `nxtrt::runtime_error` for zero limits, an output item
/// without a `type`, a malformed function call or one with empty
/// arguments, duplicate call ids, a missing response id in continuation mode,
/// and when the last allowed turn still asks for tools (those calls are not
/// run). Tool failures do not throw; they become failed results sent back
/// to the model. Transport and observer exceptions propagate.
///
/// Cancellation: a stop request is checked before each turn and otherwise
/// reaches the transport and tool batch through the awaiting task; the
/// batch drains running tools before the stop propagates.
template<typename Transport, typename Observer>
nxtrt::task<void> run_agent(
    responses::openai_responses_request request,
    const tools::tool_registry & registry,
    Transport & transport,
    Observer & observer,
    agent_options options = {})
{
    if (options.max_turns == 0 || options.tool_concurrency == 0)
        throw nxtrt::runtime_error{"agent limits must be nonzero"};
    auto continuation = request.store;
    if constexpr (requires { Transport::supports_unstored_continuation; })
        continuation |= Transport::supports_unstored_continuation;
    for (auto & definition : tools::function_tool_definitions(registry)) {
        if (std::ranges::find(
                request.tools,
                definition.name,
                &openai::function_tool_definition::name)
            == request.tools.end())
            request.tools.push_back(std::move(definition));
    }
    if (!request.store && !tools::empty(registry)
        && std::ranges::find(request.include, "reasoning.encrypted_content")
               == request.include.end())
        request.include.push_back("reasoning.encrypted_content");
    auto history = responses::input_items_from_request(request);

    for (std::size_t turn = 0; turn < options.max_turns; ++turn) {
        nxtrt::throw_if_stop_requested();
        auto response = co_await transport(request, observer);
        auto calls = std::vector<tools::function_call>{};
        for (const auto & item : response.output_items) {
            auto type = tools::json_string_member(item.str, "type");
            if (!type)
                throw nxtrt::runtime_error{"response output item missing type"};
            if (*type != "function_call")
                continue;
            // Copy for parsing; canonical items belong to the transcript.
            auto call = co_await tools::read_function_call_from_item(item);
            if (!call || call->arguments.empty())
                throw nxtrt::runtime_error{"malformed completed function call"};
            for (const auto & previous : calls)
                if (previous.call_id == call->call_id)
                    throw nxtrt::runtime_error{"duplicate function call id"};
            calls.push_back(std::move(*call));
        }
        if (calls.empty())
            co_return;
        if (turn + 1 == options.max_turns)
            throw nxtrt::runtime_error{"agent turn limit reached before executing tools"};
        if (continuation && response.id.empty())
            throw nxtrt::runtime_error{"tool response missing response id"};
        for (const auto & call : calls)
            co_await observer.tool_started(call);
        auto results = co_await tools::run_function_tool_batch(
            registry, std::move(calls), options.tool_concurrency);
        for (const auto & result : results)
            co_await observer.tool_finished(result);
        auto outputs = tools::output_items_from_results(results);
        request.input.clear();
        if (continuation) {
            request.previous_response_id = std::move(response.id);
            request.input_items = std::move(outputs);
        } else {
            for (auto & item : response.output_items)
                history.push_back(std::move(item));
            for (auto & item : outputs)
                history.push_back(std::move(item));
            request.input_items = history;
        }
    }
}

} // namespace nxtai
