#pragma once

#include <nxtai/openai_types.hpp>
#include <nxt/http.hpp>
#include <nxtio/async.hpp>
#include <nxtio/http.hpp>

#include <glaze/glaze_exceptions.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace nxt::ai::responses {

/// Error raised for malformed Responses requests, HTTP failures, and stream
/// parse errors.
struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Parameters for one OpenAI Responses API request.
struct openai_responses_request
{
    /// Bearer token used for the Authorization header.
    std::string api_key;
    /// Model identifier sent as the `model` field.
    std::string model = "gpt-5-mini";
    /// Plain text prompt used when `input_items` is empty.
    std::string input;
    /// Structured Responses `input` array for multi-turn/stateless calls.
    std::vector<openai::raw_json> input_items;
    /// Tool definitions in Responses function-tool schema.
    std::vector<openai::raw_json> tools;
    /// Extra response fields requested through the `include` option.
    std::vector<std::string> include;
    /// Server-side response id to continue when `store` is true.
    std::string previous_response_id;
    /// Upper bound for generated output tokens.
    std::size_t max_output_tokens = 6000;
    /// Reasoning effort string accepted by the selected model.
    std::string reasoning_effort = "medium";
    /// Optional reasoning summary mode.
    std::string reasoning_summary;
    /// Whether OpenAI should persist the response for server-side continuity.
    bool store = false;
};

struct user_input_item
{
    std::string role = "user";
    std::string content;
};

struct reasoning_options
{
    std::optional<std::string> effort;
    std::optional<std::string> summary;
};

struct openai_responses_body_payload
{
    std::string model;
    bool stream = true;
    bool store = false;
    std::size_t max_output_tokens = 0;
    openai::raw_json input;
    std::optional<std::vector<openai::raw_json>> tools;
    std::optional<std::vector<std::string>> include;
    std::optional<std::string> previous_response_id;
    std::optional<reasoning_options> reasoning;
};

/// One decoded server-sent event from a streaming Responses request.
struct stream_event
{
    /// SSE event type, such as `response.output_item.added`.
    std::string type;
    /// Original unparsed `data` field text.
    std::string data;

    /// Read the event's JSON data into one typed payload projection.
    template<typename T, auto Opts = openai::json_read_opts>
    [[nodiscard]] T read() const
    {
        auto payload = T{};
        glz::ex::read<Opts>(payload, data);
        return payload;
    }
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

/// Serialize a request into a JSON body for `POST /v1/responses`.
[[nodiscard]] inline std::string
openai_responses_body(const openai_responses_request & request)
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
