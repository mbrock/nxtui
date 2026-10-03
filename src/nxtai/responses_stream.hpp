#pragma once

#include <nxt/json.hpp>
#include <nxtai/openai_types.hpp>
#include <nxtai/tool_json.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxtai::responses {

// Preserve complete JSON values, including fields unknown to this client.
// In particular, reasoning items and message phase must survive continuation.
namespace detail {
using kind = nxt::json::token_kind;

inline nxtrt::task<nxt::json::token> token(nxt::json::string_reader & in)
{
    auto value = co_await nxt::json::read_token(in);
    if (!value)
        throw nxtrt::runtime_error{"truncated Responses JSON"};
    co_return std::move(*value);
}

inline nxtrt::task<void> expect(nxt::json::string_reader & in, kind expected)
{
    if ((co_await token(in)).kind != expected)
        throw nxtrt::runtime_error{"malformed Responses JSON"};
}

inline nxtrt::task<void> value(
    nxt::json::string_reader & in, nxt::json::token first, unsigned depth = 0)
{
    if (depth > 128)
        throw nxtrt::runtime_error{"Responses JSON nesting limit"};
    if (first.kind == kind::object_begin || first.kind == kind::array_begin) {
        auto object = first.kind == kind::object_begin;
        auto end = object ? kind::object_end : kind::array_end;
        auto next = co_await token(in);
        if (next.kind == end)
            co_return;
        while (true) {
            if (object) {
                if (next.kind != kind::string)
                    throw nxtrt::runtime_error{"malformed Responses JSON key"};
                co_await expect(in, kind::colon);
                next = co_await token(in);
            }
            co_await value(in, std::move(next), depth + 1);
            next = co_await token(in);
            if (next.kind == end)
                co_return;
            if (next.kind != kind::comma)
                throw nxtrt::runtime_error{"malformed Responses JSON separator"};
            next = co_await token(in);
        }
    }
    if (first.kind != kind::string && first.kind != kind::number
        && first.kind != kind::boolean && first.kind != kind::null)
        throw nxtrt::runtime_error{"malformed Responses JSON value"};
}

inline nxtrt::task<void> end(nxt::json::string_reader & in)
{
    if (co_await nxt::json::read_token(in))
        throw nxtrt::runtime_error{"trailing Responses JSON"};
}

inline nxtrt::task<std::map<std::string, std::string>> object(std::string_view raw)
{
    auto in = nxt::json::string_reader{.input = raw};
    co_await expect(in, kind::object_begin);
    auto fields = std::map<std::string, std::string>{};
    auto next = co_await token(in);
    while (next.kind != kind::object_end) {
        if (next.kind != kind::string)
            throw nxtrt::runtime_error{"malformed Responses JSON key"};
        auto key = std::move(next.text);
        co_await expect(in, kind::colon);
        auto start = in.offset;
        co_await value(in, co_await token(in));
        if (!fields.emplace(std::move(key), raw.substr(start, in.offset - start)).second)
            throw nxtrt::runtime_error{"duplicate Responses JSON key"};
        next = co_await token(in);
        if (next.kind == kind::object_end)
            break;
        if (next.kind != kind::comma)
            throw nxtrt::runtime_error{"malformed Responses JSON object"};
        next = co_await token(in);
        if (next.kind == kind::object_end)
            throw nxtrt::runtime_error{"trailing Responses JSON comma"};
    }
    co_await end(in);
    co_return fields;
}

inline nxtrt::task<std::vector<openai::raw_json>> array(std::string_view raw)
{
    auto in = nxt::json::string_reader{.input = raw};
    co_await expect(in, kind::array_begin);
    auto items = std::vector<openai::raw_json>{};
    auto start = in.offset;
    auto next = co_await token(in);
    while (next.kind != kind::array_end) {
        co_await value(in, std::move(next));
        items.push_back({std::string{raw.substr(start, in.offset - start)}});
        next = co_await token(in);
        if (next.kind == kind::array_end)
            break;
        if (next.kind != kind::comma)
            throw nxtrt::runtime_error{"malformed Responses JSON array"};
        start = in.offset;
        next = co_await token(in);
        if (next.kind == kind::array_end)
            throw nxtrt::runtime_error{"trailing Responses JSON comma"};
    }
    co_await end(in);
    co_return items;
}

inline const std::string & required(
    const std::map<std::string, std::string> & fields, const char * key)
{
    auto it = fields.find(key);
    if (it == fields.end())
        throw nxtrt::runtime_error{std::string{"Responses event missing "} + key};
    return it->second;
}
} // namespace detail

struct response_result
{
    std::string id;
    std::vector<openai::raw_json> output_items;
};

struct stream_decoder
{
    std::optional<response_result> completed;

    // Deltas are presentation only. The terminal snapshot owns canonical,
    // ordered items, regardless of how item/argument deltas were interleaved.
    nxtrt::task<std::optional<std::string>> accept(
        std::string_view type, std::string_view data)
    {
        if (completed)
            throw nxtrt::runtime_error{"Responses event after completion"};
        auto fields = co_await detail::object(data);
        auto payload_type = tools::json_string_member(data, "type");
        if (!payload_type || *payload_type != type)
            throw nxtrt::runtime_error{"Responses event type mismatch"};
        if (type == "error" || type == "response.failed"
            || type == "response.incomplete")
            throw nxtrt::runtime_error{
                "OpenAI Responses " + std::string{type} + ": " + std::string{data}};
        if (type == "response.output_text.delta" || type == "response.refusal.delta") {
            auto delta = tools::json_string_member(data, "delta");
            if (!delta)
                throw nxtrt::runtime_error{"Responses text event missing delta"};
            co_return delta;
        }
        if (type == "response.completed") {
            auto & raw = detail::required(fields, "response");
            auto response = co_await detail::object(raw);
            auto id = tools::json_string_member(raw, "id");
            auto status = tools::json_string_member(raw, "status");
            if (!id || id->empty() || status != "completed")
                throw nxtrt::runtime_error{"invalid completed Responses snapshot"};
            completed = response_result{
                std::move(*id),
                co_await detail::array(detail::required(response, "output"))};
        }
        // Item additions, argument deltas, reasoning summaries, and new
        // informational event types do not determine turn completion.
        co_return std::nullopt;
    }

    response_result finish()
    {
        if (!completed)
            throw nxtrt::runtime_error{"OpenAI Responses stream ended before completion"};
        return std::move(*completed);
    }
};

} // namespace nxtai::responses
