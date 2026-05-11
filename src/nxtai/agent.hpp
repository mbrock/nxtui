#pragma once

#include <nxtai/responses.hpp>
#include <nxtai/tools.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::agent {

/// Summary gathered while consuming one Responses stream.
struct response_stream_result
{
    /// Function calls discovered in completed output items.
    std::vector<tools::function_call> function_calls;
    /// Complete output items that should be preserved for stateless turns.
    std::vector<nlohmann::json> output_items;
    /// Most recent response id observed in the stream.
    std::optional<std::string> response_id;
    /// True when the stream reached `response.completed`.
    bool completed = false;
};

/// Result of consuming one output item from the event stream.
struct output_item_result
{
    /// Parsed function call, if the output item was a function call.
    std::optional<tools::function_call> call;
    /// Complete output item JSON, when one was available.
    std::optional<nlohmann::json> item;
};

/// Test whether an SSE event has the requested Responses event type.
[[nodiscard]] inline bool
is_event(const responses::stream_event & event, std::string_view type)
{
    return event.type == type;
}

/// Extract the event payload's `item` object when present.
[[nodiscard]] inline std::optional<nlohmann::json>
output_item_from_event(const responses::stream_event & event)
{
    if (auto it = event.payload.find("item");
        it != event.payload.end() && it->is_object())
        return *it;
    return std::nullopt;
}

/// Return the output item type carried by an event, or an empty string.
[[nodiscard]] inline std::string
output_item_type(const responses::stream_event & event)
{
    if (auto it = event.payload.find("item");
        it != event.payload.end() && it->is_object())
        return it->value("type", std::string{});
    return {};
}

/// Ask Responses to include encrypted reasoning content in output items.
inline void request_encrypted_reasoning(
    responses::openai_responses_request & request)
{
    request.include = nlohmann::json::array({"reasoning.encrypted_content"});
}

/// Attach tool definitions and stateless-continuation includes to a request.
inline void prepare_tool_request(
    responses::openai_responses_request & request,
    const std::vector<tools::function_tool> & tool_list)
{
    if (tool_list.empty())
        return;

    request.tools = tools::function_tool_definitions(tool_list);
    if (!request.store)
        request_encrypted_reasoning(request);
}

/// Append model output items followed by tool outputs to a stateless input.
inline void append_stateless_turn(
    nlohmann::json & input,
    std::vector<nlohmann::json> output_items,
    std::vector<nlohmann::json> tool_outputs)
{
    for (auto & item : output_items)
        input.push_back(std::move(item));
    for (auto & output : tool_outputs)
        input.push_back(std::move(output));
}

} // namespace nxt::ai::agent
