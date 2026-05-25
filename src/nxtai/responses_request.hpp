#pragma once

#include <nxt/http.hpp>
#include <nxtai/openai_types.hpp>

#include <glaze/glaze_exceptions.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nxt::ai::responses {

/// Parameters for one OpenAI Responses API request.
struct openai_responses_request
{
    /// Bearer token used for the Authorization header.
    std::string api_key = {};
    /// Model identifier sent as the `model` field.
    std::string model = "gpt-5-mini";
    /// Plain text prompt used when `input_items` is empty.
    std::string input = {};
    /// Structured Responses `input` array for multi-turn/stateless calls.
    std::vector<openai::raw_json> input_items = {};
    /// Tool definitions in Responses function-tool schema.
    std::vector<openai::function_tool_definition> tools = {};
    /// Extra response fields requested through the `include` option.
    std::vector<std::string> include = {};
    /// Server-side response id to continue when `store` is true.
    std::string previous_response_id = {};
    /// Upper bound for generated output tokens.
    std::size_t max_output_tokens = 6000;
    /// Reasoning effort string accepted by the selected model.
    std::string reasoning_effort = "medium";
    /// Optional reasoning summary mode.
    std::string reasoning_summary = {};
    /// Whether OpenAI should persist the response for server-side continuity.
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

/// Return the structured input array represented by a request.
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

/// Build the typed payload sent as JSON to `POST /v1/responses`.
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

/// Serialize a request into a JSON body for `POST /v1/responses`.
[[nodiscard]] inline std::string
openai_responses_body(const openai_responses_request & request)
{
    auto body = openai_responses_body_payload_from_request(request);
    return glz::ex::write_json(body);
}

/// Build the HTTP request envelope for the OpenAI Responses endpoint.
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
