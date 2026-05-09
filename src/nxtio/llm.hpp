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

struct stream_complete : std::exception
{
    [[nodiscard]] const char * what() const noexcept override
    {
        return "stream complete";
    }
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
            },
        .body = std::move(body),
    };
}

template<typename OnEvent>
nxt::task<bool>
emit_sse_event(nxt::http::server_sent_event const & sse, OnEvent & on_event)
{
    if (sse.data == "[DONE]")
        co_return true;

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(sse.data);
    } catch (const nlohmann::json::exception & e) {
        throw protocol_error{
            "OpenAI Responses stream sent invalid JSON: "
            + std::string{e.what()}};
    }

    co_await on_event(stream_event{
        .type = sse.type,
        .payload = std::move(payload),
        .raw = sse.data,
    });

    co_return sse.type == "response.completed"
        || sse.type == "response.incomplete" || sse.type == "response.failed";
}

template<typename Transport, typename OnEvent>
nxt::task<> stream_openai_responses_over(
    Transport & transport,
    const openai_responses_request & request,
    OnEvent on_event,
    std::stop_token stop = {})
{
    if (request.api_key.empty())
        throw protocol_error{"OPENAI_API_KEY is empty"};
    if (request.input.empty())
        throw protocol_error{"OpenAI Responses input is empty"};

    auto response_buffer = std::array<std::byte, 16 * 1024>{};
    auto reader =
        nxt::io::byte_reader{transport, std::span{response_buffer}, stop};
    auto response = co_await nxt::io::http::send_request(
        transport, reader, openai_responses_http_request(request));
    auto status = nxt::io::http::response_status_text(response.head);

    if (!nxt::io::http::response_status_is_success(response.head)) {
        auto body =
            co_await nxt::io::http::read_response_text(reader, response);
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

    auto on_sse_event = [&](nxt::http::server_sent_event event) -> nxt::task<> {
        if (co_await emit_sse_event(event, on_event))
            throw stream_complete{};
        co_return;
    };

    try {
        co_await nxt::io::http::read_sse_response(
            reader, response, on_sse_event);
    } catch (const stream_complete &) {
        co_return;
    }
}

} // namespace nxt::io::llm
