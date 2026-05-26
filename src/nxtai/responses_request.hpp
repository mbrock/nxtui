#pragma once

#include <nxt/http.hpp>
#include <nxt/json.hpp>
#include <nxtai/openai_types.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nxtai::responses {

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
