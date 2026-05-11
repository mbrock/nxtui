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

struct response_stream_result
{
    std::vector<tools::function_call> function_calls;
    std::vector<nlohmann::json> output_items;
    std::optional<std::string> response_id;
    bool completed = false;
};

struct output_item_result
{
    std::optional<tools::function_call> call;
    std::optional<nlohmann::json> item;
};

[[nodiscard]] inline bool
is_event(const responses::stream_event & event, std::string_view type)
{
    return event.type == type;
}

[[nodiscard]] inline std::optional<nlohmann::json>
output_item_from_event(const responses::stream_event & event)
{
    if (auto it = event.payload.find("item");
        it != event.payload.end() && it->is_object())
        return *it;
    return std::nullopt;
}

[[nodiscard]] inline std::string
output_item_type(const responses::stream_event & event)
{
    if (auto it = event.payload.find("item");
        it != event.payload.end() && it->is_object())
        return it->value("type", std::string{});
    return {};
}

inline void request_encrypted_reasoning(
    responses::openai_responses_request & request)
{
    request.include = nlohmann::json::array({"reasoning.encrypted_content"});
}

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
