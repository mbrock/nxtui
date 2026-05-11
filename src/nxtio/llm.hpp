#pragma once

#include <nxt/http.hpp>
#include <nxtio/async.hpp>
#include <nxtio/http.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace nxt::io::llm {

struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct openai_responses_request
{
    std::string api_key;
    std::string model = "gpt-5-mini";
    std::string input;
    std::size_t max_output_tokens = 6000;
    std::string reasoning_effort = "medium";
    std::string reasoning_summary;
    bool store = false;
};

struct stream_event
{
    std::string type;
    nlohmann::json payload;
    std::string raw;
};

[[nodiscard]] inline nlohmann::json
openai_responses_body(const openai_responses_request & request)
{
    auto body = nlohmann::json{
        {"model", request.model},
        {"input", request.input},
        {"stream", true},
        {"store", request.store},
        {"max_output_tokens", request.max_output_tokens},
    };

    if (!request.reasoning_effort.empty()
        || !request.reasoning_summary.empty()) {
        auto reasoning = nlohmann::json::object();
        if (!request.reasoning_effort.empty())
            reasoning["effort"] = request.reasoning_effort;
        if (!request.reasoning_summary.empty())
            reasoning["summary"] = request.reasoning_summary;
        body["reasoning"] = std::move(reasoning);
    }

    return body;
}

[[nodiscard]] inline nxt::http::request
openai_responses_http_request(const openai_responses_request & request)
{
    auto body = openai_responses_body(request).dump();
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

// Pull-shaped OpenAI Responses SSE stream.  Construct with a transport,
// call connect() once to send the request and read the response head, then
// drive the stream by calling next() until it returns nullopt.  Errors
// propagate as exceptions from connect() or next().
template<typename Transport>
class openai_response_stream
{
public:
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

    nxt::task<> connect(const openai_responses_request & request)
    {
        if (request.api_key.empty())
            throw protocol_error{"OPENAI_API_KEY is empty"};
        if (request.input.empty())
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

            nlohmann::json payload;
            try {
                payload = nlohmann::json::parse(sse->data);
            } catch (const nlohmann::json::exception & e) {
                throw protocol_error{
                    "OpenAI Responses stream sent invalid JSON: "
                    + std::string{e.what()}};
            }

            auto terminal = sse->type == "response.completed"
                            || sse->type == "response.incomplete"
                            || sse->type == "response.failed";

            auto event = stream_event{
                .type = std::move(sse->type),
                .payload = std::move(payload),
                .raw = std::move(sse->data),
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

} // namespace nxt::io::llm
