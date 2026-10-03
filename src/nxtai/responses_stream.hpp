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

// Strict JSON helpers that slice complete values out of event payloads
// without reinterpreting them, so fields unknown to this client (reasoning
// items, message phase) survive continuation.
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

/// A completed response: its id and its output items, in order, as raw
/// JSON exactly as the server sent them.
struct response_result
{
    /// Response id, usable as `previous_response_id`.
    std::string id;
    /// Output items from the `response.completed` snapshot.
    std::vector<openai::raw_json> output_items;
};

/// Consumes Responses stream events one at a time and keeps the completed
/// response.
///
/// Feed it the `event` name and `data` of each server-sent event. Text and
/// refusal deltas are returned for display as they arrive; every other
/// event (item additions, argument deltas, reasoning summaries, unknown
/// types) is validated as JSON and otherwise ignored. Only the
/// `response.completed` snapshot decides the result, so interleaved deltas
/// cannot reorder or invent output items.
///
/// The decoder is a plain value with no I/O; `nxtllm` drives it from an
/// SSE parser.
struct stream_decoder
{
    /// Set once `response.completed` has been accepted.
    std::optional<response_result> completed;

    /// Decode one event. Returns the text of an `output_text` or `refusal`
    /// delta, otherwise `std::nullopt`.
    ///
    /// Throws `nxtrt::runtime_error` when `data` is not a single valid JSON
    /// object (duplicate keys, trailing data, nesting over 128 levels), when
    /// its `type` member differs from `type`, for `error`,
    /// `response.failed`, and `response.incomplete` events (with the payload
    /// in the message), for a `response.completed` whose response lacks an
    /// id, has a status other than `completed`, or has no `output` array,
    /// and for any event after completion. The awaitable only parses; it
    /// does not suspend on I/O.
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

    /// Take the completed response.
    /// @throws nxtrt::runtime_error if the stream ended before
    /// `response.completed`.
    response_result finish()
    {
        if (!completed)
            throw nxtrt::runtime_error{"OpenAI Responses stream ended before completion"};
        return std::move(*completed);
    }
};

} // namespace nxtai::responses
