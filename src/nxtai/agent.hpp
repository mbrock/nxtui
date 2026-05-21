#pragma once

#include <nxtai/responses.hpp>
#include <nxtai/tools.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::agent {

/// Summary gathered while consuming one Responses stream.
struct response_stream_result
{
    /// Complete output items that should be preserved for stateless turns.
    std::vector<openai::raw_json> output_items;
    /// Most recent response id observed in the stream.
    std::optional<std::string> response_id;
    /// True when the stream reached `response.completed`.
    bool completed = false;
};

/// Test whether an SSE event has the requested Responses event type.
[[nodiscard]] inline bool
is_event(const responses::stream_event & event, std::string_view type)
{
    return event.type == type;
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
    std::vector<openai::raw_json> output_items,
    std::vector<nlohmann::json> tool_outputs)
{
    for (auto & item : output_items)
        input.push_back(nlohmann::json::parse(item.str));
    for (auto & output : tool_outputs)
        input.push_back(std::move(output));
}

/// Extract assistant message text blocks from completed response items.
[[nodiscard]] inline std::vector<std::string>
message_blocks_from_items(const std::vector<openai::raw_json> & output_items)
{
    auto messages = std::vector<std::string>{};
    for (const auto & raw_item : output_items) {
        if (raw_item.str.empty())
            continue;

        auto item = openai::message_item{};
        if (auto ec =
                glz::read<openai::json_read_opts>(item, raw_item.str))
            throw std::runtime_error{glz::format_error(ec, raw_item.str)};
        if (item.type != "message")
            continue;

        auto text = std::string{};
        for (const auto & part : item.content)
            if (part.type == "output_text")
                text += part.text;
        if (!text.empty())
            messages.push_back(std::move(text));
    }
    return messages;
}

/// Stateful Responses continuation rules for multi-step tool turns.
///
/// This owns only protocol state: the next request to send, the tool list, and
/// the stateless transcript needed when server-side response storage is off.
class response_continuation
{
public:
    response_continuation(
        responses::openai_responses_request request,
        std::vector<tools::function_tool> tools,
        std::size_t max_steps = 256)
        : original_request_(std::move(request))
        , request_(original_request_)
        , tools_(std::move(tools))
        , stateless_input_(responses::input_items_from_request(request_))
        , max_steps_(max_steps)
    {
        prepare_tool_request(request_, tools_);
    }

    [[nodiscard]] bool can_step() const
    {
        return step_ < max_steps_;
    }

    [[nodiscard]] const responses::openai_responses_request & request() const
    {
        return request_;
    }

    [[nodiscard]] const std::vector<tools::function_tool> & tools() const
    {
        return tools_;
    }

    [[nodiscard]] std::optional<std::string> continuation_problem(
        const response_stream_result & response,
        bool needs_tools) const
    {
        if (request_.store && needs_tools && !response.response_id)
            return "tool call response had no response id";
        return std::nullopt;
    }

    void continue_after_tools(
        response_stream_result response,
        std::vector<nlohmann::json> tool_outputs)
    {
        ++step_;

        request_ = original_request_;
        request_.input.clear();
        request_.previous_response_id.clear();

        if (request_.store) {
            request_.input_items = std::move(tool_outputs);
            if (response.response_id)
                request_.previous_response_id = *response.response_id;
        } else {
            append_stateless_turn(
                stateless_input_,
                std::move(response.output_items),
                std::move(tool_outputs));
            request_.input_items = stateless_input_;
        }

        prepare_tool_request(request_, tools_);
    }

private:
    responses::openai_responses_request original_request_;
    responses::openai_responses_request request_;
    std::vector<tools::function_tool> tools_;
    nlohmann::json stateless_input_ = nlohmann::json::array();
    std::size_t step_ = 0;
    std::size_t max_steps_ = 0;
};

} // namespace nxt::ai::agent
