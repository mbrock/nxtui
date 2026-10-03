#pragma once

#include <nxt/http.hpp>
#include <nxt/json.hpp>
#include <nxtai/openai_types.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @namespace nxtai::responses
 * OpenAI Responses API wire format: building the streaming request body
 * from an `openai_responses_request`, and decoding the server-sent events
 * of the reply into a `response_result` with `stream_decoder`.
 *
 * Nothing here does I/O. Items are kept as raw JSON (`openai::raw_json`) so
 * fields this client does not know about survive a round trip.
 */
namespace nxtai::responses {

/// Everything needed to build one `POST /v1/responses` request.
///
/// The body always asks for a streamed reply (`"stream": true`). Empty
/// strings and vectors are left out of the body, except `input`, which is
/// always sent.
struct openai_responses_request
{
    /// Bearer token for the `Authorization` header; not part of the body.
    std::string api_key = {};
    /// Model name; the default matches `nxtllm`.
    std::string model = "gpt-6-luna";
    /// Plain-text user input, sent as the `input` string when
    /// `input_items` is empty.
    std::string input = {};
    /// Input items as raw JSON objects; when non-empty they are sent as the
    /// `input` array and `input` is ignored.
    std::vector<openai::raw_json> input_items = {};
    /// Function tools offered to the model.
    std::vector<openai::function_tool_definition> tools = {};
    /// Extra output fields to include, e.g. `reasoning.encrypted_content`.
    std::vector<std::string> include = {};
    /// Continue from a stored response (used with `store`).
    std::string previous_response_id = {};
    /// Upper bound on generated tokens.
    std::size_t max_output_tokens = 6000;
    /// `reasoning.effort`; empty leaves it out.
    std::string reasoning_effort = {};
    /// `reasoning.summary`; empty leaves it out.
    std::string reasoning_summary = {};
    /// Whether the server stores the response for `previous_response_id`.
    bool store = false;
};

/// Not used by the request builder.
struct user_input_item
{
    std::string role = "user";
    std::string content = {};
};

/// Not used by the request builder.
struct reasoning_options
{
    std::optional<std::string> effort = {};
    std::optional<std::string> summary = {};
};

/// Typed mirror of the request body. Not used by `openai_responses_body`,
/// which writes JSON directly.
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

/// The request's input as a list of items: `input_items` if non-empty,
/// otherwise one `{"role":"user","content":input}` item, or nothing when
/// `input` is empty too.
[[nodiscard]] inline std::vector<openai::raw_json>
input_items_from_request(const openai_responses_request & request)
{
    if (!request.input_items.empty())
        return request.input_items;

    auto input = std::vector<openai::raw_json>{};
    if (!request.input.empty()) {
        auto json = nxt::json::writer{};
        json.character('{');
        json.key("role");
        json.string("user");
        json.character(',');
        json.key("content");
        json.string(request.input);
        json.character('}');
        input.emplace_back(std::move(json.out));
    }
    return input;
}

/// Write `values` as a JSON array of strings.
inline void write_string_array(
    nxt::json::writer & json,
    const std::vector<std::string> & values)
{
    json.character('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0)
            json.character(',');
        json.string(values[i]);
    }
    json.character(']');
}

/// Write already-serialized JSON values as a JSON array, verbatim.
inline void write_raw_json_array(
    nxt::json::writer & json,
    const std::vector<openai::raw_json> & values)
{
    json.character('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0)
            json.character(',');
        json.raw(values[i].str);
    }
    json.character(']');
}

/// Write one function tool definition object; `parameters` is copied
/// verbatim.
inline void write_tool_definition(
    nxt::json::writer & json,
    const openai::function_tool_definition & tool)
{
    json.character('{');
    json.key("type");
    json.string(tool.type);
    json.character(',');
    json.key("name");
    json.string(tool.name);
    json.character(',');
    json.key("description");
    json.string(tool.description);
    json.character(',');
    json.key("parameters");
    json.raw(tool.parameters.str);
    json.character(',');
    json.key("strict");
    json.boolean(tool.strict);
    json.character('}');
}

inline void write_tools_array(
    nxt::json::writer & json,
    const std::vector<openai::function_tool_definition> & tools)
{
    json.character('[');
    for (std::size_t i = 0; i < tools.size(); ++i) {
        if (i != 0)
            json.character(',');
        write_tool_definition(json, tools[i]);
    }
    json.character(']');
}

/// Write the `input` member: the `input_items` array, or the `input`
/// string when there are no items.
inline void write_responses_input(
    nxt::json::writer & json,
    const openai_responses_request & request)
{
    json.key("input");
    if (request.input_items.empty()) {
        json.string(request.input);
    } else {
        write_raw_json_array(json, request.input_items);
    }
}

/// Serialize `request` as the JSON body of a streaming Responses request.
///
/// Raw JSON in `input_items` and tool `parameters` is inserted without
/// validation, so it must already be valid JSON.
[[nodiscard]] inline std::string
openai_responses_body(const openai_responses_request & request)
{
    auto json = nxt::json::writer{};
    json.character('{');
    json.key("model");
    json.string(request.model);
    json.character(',');
    json.key("stream");
    json.boolean(true);
    json.character(',');
    json.key("store");
    json.boolean(request.store);
    json.character(',');
    json.key("max_output_tokens");
    json.number(request.max_output_tokens);
    json.character(',');
    write_responses_input(json, request);

    if (!request.tools.empty()) {
        json.character(',');
        json.key("tools");
        write_tools_array(json, request.tools);
    }

    if (!request.include.empty()) {
        json.character(',');
        json.key("include");
        write_string_array(json, request.include);
    }

    if (!request.previous_response_id.empty()) {
        json.character(',');
        json.key("previous_response_id");
        json.string(request.previous_response_id);
    }

    if (!request.reasoning_effort.empty() || !request.reasoning_summary.empty()) {
        json.character(',');
        json.key("reasoning");
        json.character('{');
        auto need_comma = false;
        if (!request.reasoning_effort.empty()) {
            json.key("effort");
            json.string(request.reasoning_effort);
            need_comma = true;
        }
        if (!request.reasoning_summary.empty()) {
            if (need_comma)
                json.character(',');
            json.key("summary");
            json.string(request.reasoning_summary);
        }
        json.character('}');
    }

    json.character('}');
    return std::move(json.out);
}

/// Complete `POST https://api.openai.com/v1/responses` request with SSE
/// `Accept`, JSON body, and bearer authorization. `nxtllm` builds its own
/// `nxtrt::http::request` instead, adding `Accept-Encoding` and
/// `Connection: close`.
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

} // namespace nxtai::responses
