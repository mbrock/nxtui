#pragma once

#include <nxtai/openai_types.hpp>
#include <nxtai/responses_request.hpp>
#include <nxtio/async.hpp>
#include <nxtio/http.hpp>
#include <nxtio/stacktrace.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace nxt::ai::responses {

/// Error raised for malformed Responses requests, HTTP failures, and stream
/// parse errors.
struct protocol_error : nxt::io::runtime_error
{
    using nxt::io::runtime_error::runtime_error;
};

/// One decoded server-sent event from a streaming Responses request.
struct stream_event
{
    /// SSE event type, such as `response.output_item.added`.
    std::string type = {};
    /// Original unparsed `data` field text.
    std::string data = {};

    /// Read the event's JSON data into one typed payload projection.
    template<typename T, auto Opts = openai::json_read_opts>
    [[nodiscard]] T read() const
    {
        auto payload = T{};
        glz::ex::read<Opts>(payload, data);
        return payload;
    }
};

/// Pull-shaped OpenAI Responses SSE stream.
///
/// Construct with a connected transport, call `connect()` once to send the
/// request and validate the response head, then drive the stream by calling
/// `next()` until it returns `std::nullopt`.
template<typename Transport>
class openai_response_stream
{
public:
    /// Bind the stream to an already-connected byte transport.
    explicit openai_response_stream(
        Transport & transport, std::stop_token stop = {})
        : transport_(&transport)
        , stop_(std::move(stop))
        , reader_(transport, std::span{head_buffer_}, stop_)
    {
    }

    openai_response_stream(const openai_response_stream &) = delete;
    openai_response_stream & operator=(const openai_response_stream &) = delete;
    openai_response_stream(openai_response_stream &&) = delete;
    openai_response_stream & operator=(openai_response_stream &&) = delete;

    /// Send the request and prepare to read its `text/event-stream` body.
    nxt::task<> connect(const openai_responses_request & request)
    {
        if (request.api_key.empty())
            throw protocol_error{"OPENAI_API_KEY is empty"};
        if (request.input.empty() && request.input_items.empty())
            throw protocol_error{"OpenAI Responses input is empty"};

        auto response = co_await nxt::io::http::send_request(
            *transport_, reader_, openai_responses_http_request(request));

        if (!nxt::io::http::response_status_is_success(response.head)) {
            auto status = nxt::io::http::response_status_text(response.head);
            auto body = co_await nxt::io::http::read_response_text(
                reader_, response);
            throw protocol_error{
                "OpenAI Responses HTTP error: " + status + ": " + body};
        }

        if (!nxt::io::http::response_content_type_is(
                response.head, "text/event-stream")) {
            auto content_type =
                nxt::io::http::header_value(response.head, "content-type")
                    .value_or("<missing>");
            throw protocol_error{
                "OpenAI Responses expected text/event-stream, got "
                + std::string{content_type}};
        }

        body_.emplace(reader_, response.head);
        body_reader_.emplace(*body_, std::span{body_buffer_}, stop_);
    }

    /// Read the next decoded event, or `std::nullopt` after stream end.
    nxt::task<std::optional<stream_event>> next()
    {
        if (done_ || !body_reader_)
            co_return std::nullopt;

        while (true) {
            auto sse = co_await nxt::io::http::parse_sse_event(*body_reader_);
            if (!sse) {
                done_ = true;
                co_return std::nullopt;
            }

            if (sse->data == "[DONE]") {
                done_ = true;
                co_return std::nullopt;
            }

            auto terminal = sse->type == "response.completed"
                            || sse->type == "response.incomplete"
                            || sse->type == "response.failed";

            auto event = stream_event{
                .type = std::move(sse->type),
                .data = std::move(sse->data),
            };

            if (terminal)
                done_ = true;
            co_return event;
        }
    }

private:
    using head_reader_t = nxt::io::byte_reader<Transport>;
    using body_t = nxt::io::http::http_body_reader<head_reader_t>;
    using body_reader_t = nxt::io::byte_reader<body_t>;

    Transport * transport_;
    std::stop_token stop_;
    std::array<std::byte, 8 * 1024> head_buffer_{};
    head_reader_t reader_;
    std::array<std::byte, 16 * 1024> body_buffer_{};
    std::optional<body_t> body_;
    std::optional<body_reader_t> body_reader_;
    bool done_ = false;
};

} // namespace nxt::ai::responses
