#pragma once

#include <nxt/http.hpp>
#include <nxtio/buffers.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "nxtio/async.hpp"

namespace nxt::io::http {

struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct url
{
    bool tls = false;
    std::string host;
    std::string port;
    std::string target = "/";
};

url parse_url(std::string_view text);
std::uint16_t parse_port(std::string_view text);
bool is_default_port(const url & url);

nxt::http::response_head parse_response_head(std::span<const std::byte> bytes);
std::string_view as_text(std::span<const std::byte> bytes);

std::optional<std::string_view>
header_value(const nxt::http::response_head & response, std::string_view name);

bool has_header_token(
    const nxt::http::response_head & response,
    std::string_view name,
    std::string_view token);

std::optional<std::size_t>
response_content_length(const nxt::http::response_head & response);

bool response_is_chunked(const nxt::http::response_head & response);
bool response_status_is_success(const nxt::http::response_head & response);

bool response_content_type_is(
    const nxt::http::response_head & response,
    std::string_view expected);

std::string response_status_text(const nxt::http::response_head & response);

struct response_start
{
    nxt::http::response_head head;
};

template<typename Reader, typename OnChunk>
nxt::task<> read_content_length(
    Reader & reader,
    std::size_t content_length,
    OnChunk on_chunk)
{
    auto remaining = content_length;
    while (remaining > 0) {
        auto available = reader.buffered();
        if (!available.empty()) {
            auto n = std::min(remaining, available.size());
            co_await on_chunk(available.first(n));
            reader.toss(n);
            remaining -= n;
            continue;
        }

        if (co_await reader.fill_more() == 0)
            throw protocol_error{"unexpected end of input"};
    }
}

std::size_t parse_chunk_size(std::span<const std::byte> line);

template<typename Reader>
nxt::task<> read_expected_crlf(Reader & reader)
{
    auto crlf = co_await reader.take_until("\r\n");
    if (!crlf.empty())
        throw protocol_error{"chunk data was not followed by CRLF"};
}

template<typename Reader, typename OnChunk>
nxt::task<> read_chunked(
    Reader & reader,
    OnChunk on_chunk)
{
    while (true) {
        auto line = co_await reader.take_until("\r\n");
        auto chunk_size = parse_chunk_size(line);

        if (chunk_size == 0) {
            auto trailers = co_await reader.take_until("\r\n");
            if (!trailers.empty())
                throw protocol_error{"chunk trailers are not supported"};
            co_return;
        }

        co_await read_content_length(reader, chunk_size, on_chunk);
        co_await read_expected_crlf(reader);
    }
}

template<typename Reader, typename OnChunk>
nxt::task<> read_until_eof(Reader & reader, OnChunk on_chunk)
{
    while (true) {
        auto available = reader.buffered();
        if (!available.empty()) {
            co_await on_chunk(available);
            reader.toss(available.size());
            continue;
        }

        if (co_await reader.fill_more() == 0)
            co_return;
    }
}

template<typename Reader, typename OnChunk>
nxt::task<> read_response_body_chunks(
    Reader & reader,
    const nxt::http::response_head & response,
    OnChunk on_chunk)
{
    // Body-stage dispatcher.  The byte reader owns read-ahead from the response
    // head stage, and this layer strips only HTTP transfer framing.
    if (response_is_chunked(response)) {
        co_await read_chunked(reader, on_chunk);
    } else if (auto length = response_content_length(response)) {
        co_await read_content_length(reader, *length, on_chunk);
    } else {
        co_await read_until_eof(reader, on_chunk);
    }
}

template<typename Transport, typename Reader>
nxt::task<response_start> send_request(
    Transport & transport,
    Reader & reader,
    const nxt::http::request & request)
{
    co_await transport.write_all(nxt::http::serialize(request));

    // Stage 1: parse only through the response head.  Any bytes already read
    // beyond the "\r\n\r\n" boundary stay buffered in reader for the selected
    // body parser.
    auto head = co_await reader.take_until("\r\n\r\n");
    auto response = parse_response_head(head);

    co_return response_start{
        .head = std::move(response),
    };
}

template<typename Reader, typename OnChunk>
nxt::task<> read_response_body(
    Reader & reader,
    const response_start & response,
    OnChunk on_chunk)
{
    co_await read_response_body_chunks(
        reader,
        response.head,
        on_chunk);
}

template<typename Reader>
nxt::task<std::string> read_response_text(
    Reader & reader,
    const response_start & response)
{
    // Convenience semantic stage for callers that know the body should be
    // treated as opaque bytes/text, commonly error responses.
    auto body = std::string{};
    auto collect = [&](std::span<const std::byte> chunk) -> nxt::task<> {
        body += as_text(chunk);
        co_return;
    };

    co_await read_response_body(reader, response, collect);
    co_return body;
}

template<typename Reader, typename OnEvent>
nxt::task<> read_sse_response(
    Reader & reader,
    const response_start & response,
    OnEvent on_event)
{
    // Convenience semantic stage for callers that already decided this response
    // is an SSE stream.  The HTTP transfer framing is stripped by
    // read_response_body; only complete SSE events are emitted here.
    auto parser = nxt::http::server_sent_event_parser{};
    auto on_chunk = [&](std::span<const std::byte> chunk) -> nxt::task<> {
        for (auto & event : parser.feed(as_text(chunk)))
            co_await on_event(std::move(event));
    };

    co_await read_response_body(reader, response, on_chunk);

    for (auto & event : parser.close())
        co_await on_event(std::move(event));
}

} // namespace nxt::io::http
