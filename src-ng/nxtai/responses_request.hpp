#pragma once

#include <nxt/http.hpp>
#include <nxtai/openai_types.hpp>

#include <glaze/glaze_exceptions.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nxt::ai::responses {

struct openai_responses_request
{
    std::string api_key = {};
    std::string model = "gpt-5-mini";
    std::string input = {};
    std::vector<openai::raw_json> input_items = {};
    std::vector<openai::function_tool_definition> tools = {};
    std::vector<std::string> include = {};
    std::string previous_response_id = {};
    std::size_t max_output_tokens = 6000;
    std::string reasoning_effort = "medium";
    std::string reasoning_summary = {};
    bool store = false;
};

struct user_input_item
{
    std::string role = "user";
    std::string content = {};
};

struct reasoning_options
{
    std::optional<std::string> effort = {};
    std::optional<std::string> summary = {};
};

struct openai_responses_body_payload
{
    std::string model = {};
    bool stream = true;
    bool store = false;
    std::size_t max_output_tokens = 0;
    openai::raw_json input = {};
    std::optional<std::vector<openai::function_tool_definition>> tools = {};
    std::optional<std::vector<std::string>> include = {};
    std::optional<std::string> previous_response_id = {};
    std::optional<reasoning_options> reasoning = {};
};

[[nodiscard]] inline std::vector<openai::raw_json>
input_items_from_request(const openai_responses_request & request)
{
    if (!request.input_items.empty())
        return request.input_items;

    auto input = std::vector<openai::raw_json>{};
    if (!request.input.empty())
        input.emplace_back(
            glz::ex::write_json(user_input_item{.content = request.input}));
    return input;
}

[[nodiscard]] inline openai_responses_body_payload
openai_responses_body_payload_from_request(
    const openai_responses_request & request)
{
    auto body = openai_responses_body_payload{
        .model = request.model,
        .stream = true,
        .store = request.store,
        .max_output_tokens = request.max_output_tokens,
        .input =
            request.input_items.empty()
                ? openai::raw_json{glz::ex::write_json(request.input)}
                : openai::raw_json{glz::ex::write_json(request.input_items)},
    };

    if (!request.tools.empty())
        body.tools = request.tools;

    if (!request.include.empty())
        body.include = request.include;

    if (!request.previous_response_id.empty())
        body.previous_response_id = request.previous_response_id;

    auto reasoning = reasoning_options{};
    if (!request.reasoning_effort.empty())
        reasoning.effort = request.reasoning_effort;
    if (!request.reasoning_summary.empty())
        reasoning.summary = request.reasoning_summary;
    if (reasoning.effort || reasoning.summary)
        body.reasoning = std::move(reasoning);

    return body;
}

[[nodiscard]] inline std::string
openai_responses_body(const openai_responses_request & request)
{
    auto body = openai_responses_body_payload_from_request(request);
    return glz::ex::write_json(body);
}

[[nodiscard]] inline nxt::http::request
openai_responses_http_request(const openai_responses_request & request)
{
    auto body = openai_responses_body(request);
    return nxt::http::request{
        .method = "POST",
        .target = "/v1/responses",
        .host = "api.openai.com",
        .headers =
            {
                {"User-Agent", "nxtllm/0"},
                {"Accept", "text/event-stream"},
                {"Content-Type", "application/json"},
                {"Authorization", "Bearer " + request.api_key},
                {"Connection", "keep-alive"},
            },
        .body = std::move(body),
    };
}

} // namespace nxt::ai::responses
