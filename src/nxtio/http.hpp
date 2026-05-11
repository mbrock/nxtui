#pragma once

#include <nxt/http.hpp>
#include <nxtio/buffers.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "nxtio/async-core.hpp"

namespace nxt::io::http {

/// Error raised for HTTP protocol parsing and transfer-framing failures.
struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Parsed URL pieces supported by the small HTTP client.
struct url
{
    /// True for https URLs.
    bool tls = false;
    /// Hostname or address.
    std::string host;
    /// Port text; may be empty when the scheme implies a default.
    std::string port;
    /// Request target path and query.
    std::string target = "/";
};

/// Parse an HTTP or HTTPS URL into connection and request-target pieces.
url parse_url(std::string_view text);
/// Parse a decimal TCP port.
std::uint16_t parse_port(std::string_view text);
/// True when the URL uses its scheme's default port.
bool is_default_port(const url & url);

/// Parse bytes up to an HTTP response-head boundary.
nxt::http::response_head parse_response_head(std::span<const std::byte> bytes);
/// Reinterpret bytes as text.
std::string_view as_text(std::span<const std::byte> bytes);

/// Return the first matching response header value, case-insensitively.
std::optional<std::string_view>
header_value(const nxt::http::response_head & response, std::string_view name);

/// True when a comma-separated response header contains `token`.
bool has_header_token(
    const nxt::http::response_head & response,
    std::string_view name,
    std::string_view token);

/// Parsed Content-Length header, when present.
std::optional<std::size_t>
response_content_length(const nxt::http::response_head & response);

/// True when the response uses chunked transfer encoding.
bool response_is_chunked(const nxt::http::response_head & response);
/// True for 2xx response status codes.
bool response_status_is_success(const nxt::http::response_head & response);

/// True when the response Content-Type matches the expected media type.
bool response_content_type_is(
    const nxt::http::response_head & response,
    std::string_view expected);

/// Human-readable response status line.
std::string response_status_text(const nxt::http::response_head & response);

/// Response head returned after sending a request.
struct response_start
{
    /// Parsed status line and headers.
    nxt::http::response_head head;
};

/// Read exactly `content_length` bytes and pass chunks to `on_chunk`.
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

/// Parse a hexadecimal chunk-size line.
std::size_t parse_chunk_size(std::span<const std::byte> line);

/// Read and validate an expected CRLF after chunk data.
template<typename Reader>
nxt::task<> read_expected_crlf(Reader & reader)
{
    auto crlf = co_await reader.take_until("\r\n");
    if (!crlf.empty())
        throw protocol_error{"chunk data was not followed by CRLF"};
}

/// Read a chunked transfer-encoded body.
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

/// Read body bytes until the connection ends.
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

/// Dispatch to the body reader required by the response headers.
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

/// Write a request and parse the response head.
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

/// Read the response body as chunks.
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

/// Read the full response body into a string.
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

/// Source-shaped view over an HTTP response body.
///
/// It strips HTTP transfer framing (Content-Length, chunked, or
/// close-delimited) so callers see only payload bytes through `read_some`.
template<typename Reader>
class http_body_reader
{
public:
    /// Select a body-reading mode from the response headers.
    http_body_reader(Reader & upstream, const nxt::http::response_head & head)
        : upstream_(&upstream)
    {
        if (response_is_chunked(head)) {
            mode_ = mode::chunked;
        } else if (auto length = response_content_length(head)) {
            mode_ = mode::content_length;
            content_remaining_ = *length;
        } else {
            mode_ = mode::eof;
        }
    }

    /// Read unframed body bytes into `dst`.
    nxt::task<std::size_t> read_some(std::span<std::byte> dst)
    {
        if (done_ || dst.empty())
            co_return 0;

        switch (mode_) {
        case mode::content_length:
            co_return co_await read_bounded(dst);
        case mode::eof:
            co_return co_await read_eof(dst);
        case mode::chunked:
            co_return co_await read_chunked(dst);
        }
        co_return 0;
    }

private:
    enum class mode
    {
        content_length,
        chunked,
        eof,
    };

    nxt::task<std::size_t> copy_from_buffered(
        std::span<std::byte> dst, std::size_t budget)
    {
        auto buffered = upstream_->buffered();
        if (buffered.empty()) {
            auto filled = co_await upstream_->fill_more();
            if (filled == 0)
                co_return 0;
            buffered = upstream_->buffered();
        }
        auto n = std::min({buffered.size(), dst.size(), budget});
        std::memcpy(dst.data(), buffered.data(), n);
        upstream_->toss(n);
        co_return n;
    }

    nxt::task<std::size_t> read_bounded(std::span<std::byte> dst)
    {
        auto n = co_await copy_from_buffered(dst, content_remaining_);
        if (n == 0)
            throw protocol_error{"unexpected end of content-length body"};
        content_remaining_ -= n;
        if (content_remaining_ == 0)
            done_ = true;
        co_return n;
    }

    nxt::task<std::size_t> read_eof(std::span<std::byte> dst)
    {
        auto n = co_await copy_from_buffered(dst, dst.size());
        if (n == 0)
            done_ = true;
        co_return n;
    }

    nxt::task<std::size_t> read_chunked(std::span<std::byte> dst)
    {
        while (true) {
            if (chunk_remaining_ > 0) {
                auto n = co_await copy_from_buffered(dst, chunk_remaining_);
                if (n == 0)
                    throw protocol_error{"unexpected end of chunked body"};
                chunk_remaining_ -= n;
                if (chunk_remaining_ == 0) {
                    auto crlf = co_await upstream_->take_until("\r\n");
                    if (!crlf.empty())
                        throw protocol_error{
                            "chunk data was not followed by CRLF"};
                }
                co_return n;
            }

            auto line = co_await upstream_->take_until("\r\n");
            auto chunk_size = parse_chunk_size(line);
            if (chunk_size == 0) {
                auto trailers = co_await upstream_->take_until("\r\n");
                if (!trailers.empty())
                    throw protocol_error{"chunk trailers are not supported"};
                done_ = true;
                co_return 0;
            }
            chunk_remaining_ = chunk_size;
        }
    }

    Reader * upstream_;
    mode mode_ = mode::eof;
    std::size_t content_remaining_ = 0;
    std::size_t chunk_remaining_ = 0;
    bool done_ = false;
};

/// Pull one server-sent event from a buffered byte stream.
///
/// Returns `std::nullopt` on clean end-of-stream. `Reader` is expected to be
/// a `byte_reader`-shaped source with `take_until("\n")`.
template<typename Reader>
nxt::task<std::optional<nxt::http::server_sent_event>>
parse_sse_event(Reader & reader)
{
    auto event = nxt::http::server_sent_event{};
    auto have_data = false;
    auto have_fields = false;

    while (true) {
        std::span<const std::byte> raw;
        try {
            raw = co_await reader.take_until("\n");
        } catch (const end_of_stream &) {
            if (have_data) {
                if (!event.data.empty() && event.data.back() == '\n')
                    event.data.pop_back();
                co_return std::move(event);
            }
            if (have_fields)
                throw protocol_error{"unterminated server-sent event"};
            co_return std::nullopt;
        }

        if (!raw.empty() && raw.back() == std::byte{'\r'})
            raw = raw.first(raw.size() - 1);

        if (raw.empty()) {
            if (have_data) {
                if (!event.data.empty() && event.data.back() == '\n')
                    event.data.pop_back();
                co_return std::move(event);
            }
            // Blank line with no fields — separator between events; keep going.
            have_fields = false;
            continue;
        }

        have_fields = true;

        auto text = as_text(raw);
        if (text.front() == ':')
            continue;

        auto colon = text.find(':');
        auto field = colon == std::string_view::npos
                         ? text
                         : text.substr(0, colon);
        auto value = colon == std::string_view::npos
                         ? std::string_view{}
                         : text.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
            value.remove_prefix(1);

        if (field == "data") {
            event.data += value;
            event.data += '\n';
            have_data = true;
        } else if (field == "event") {
            event.type = value.empty() ? "message" : std::string{value};
        } else if (field == "id") {
            if (value.find('\0') == std::string_view::npos)
                event.id = std::string{value};
        } else if (field == "retry") {
            auto retry = 0;
            auto * first = value.data();
            auto * last = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(first, last, retry);
            if (!value.empty() && ec == std::errc{} && ptr == last)
                event.retry_ms = retry;
        }
    }
}

} // namespace nxt::io::http
