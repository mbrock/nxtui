#pragma once

#include <nxtai/responses_request.hpp>
#include <nxtai/responses_stream.hpp>
#include <nxtai/tool_batch.hpp>

namespace nxtai {

struct agent_options
{
    std::size_t max_turns = 32;
    std::size_t tool_concurrency = 1;
};

// Transport and observer are borrowed through settlement. The task owns the
// request/history; neither terminal rendering nor a second scheduler owns it.
// Transport(request, observer) returns a completed response_result.
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
    request.tools = tools::function_tool_definitions(registry);
    if (!request.store && !tools::empty(registry))
        request.include = {"reasoning.encrypted_content"};
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
        if (request.store && response.id.empty())
            throw nxtrt::runtime_error{"tool response missing response id"};
        for (const auto & call : calls)
            co_await observer.tool_started(call);
        auto results = co_await tools::run_function_tool_batch(
            registry, std::move(calls), options.tool_concurrency);
        for (const auto & result : results)
            co_await observer.tool_finished(result);
        auto outputs = tools::output_items_from_results(results);
        request.input.clear();
        if (request.store) {
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
