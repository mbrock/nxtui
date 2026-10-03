#pragma once

#include "nxtrt/buffer-core.hpp"
#include "nxtrt/buffers.hpp"
#include "nxtrt/compression.hpp"
#include "nxtrt/value-buffers.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * @namespace nxtrt::http
 * HTTP/1.1 pieces: a small client toolkit and a plain origin server.
 *
 * The client side is a set of composable parts, not a client object:
 * `parse_url`, `serialize` a `request`, write it to a socket or
 * `nxtrt::tls::tls13_client_session`, then `read_response_head` and wrap the
 * same byte feed in `response_body_decoding_reader` (de-chunked and
 * decompressed) or `http_body_reader` (de-chunked only). `sse_feed` parses a
 * `text/event-stream` body into events. Connection setup lives in
 * `nxtrt::net` (`connect_tcp`) and TLS in `nxtrt::tls`.
 *
 * The server side is `serve` in `nxtrt/http-server.hpp`: a bounded HTTP/1.1
 * server for use behind a TLS-terminating proxy. See @ref wisp for how Wisp
 * exposes both.
 */
namespace nxtrt::http {

/// Malformed or unsupported HTTP on the wire, or an unsupported URL.
struct protocol_error : runtime_error
{
    using runtime_error::runtime_error;
};

/// One header field. Order and duplicates are kept as received or given.
struct header
{
    std::string name;
    std::string value;
};

/// An `http://` or `https://` URL split by `parse_url`.
struct url
{
    bool tls = false;     ///< True for `https://`.
    std::string host;     ///< Host name or IPv4 literal, without the port.
    std::string port;     ///< Explicit port, or "80"/"443" by scheme.
    std::string target = "/"; ///< Path and query; "/" when the URL has none.
};

/// An HTTP/1.1 request message.
///
/// The client serializes it with `serialize`; `serve` fills one per request
/// and passes it to the handler, with `host` taken from the Host header and
/// `body` fully read and de-chunked.
struct request
{
    std::string method = "GET";
    std::string target = "/";
    std::string host; ///< Host header value; omitted by `serialize` if empty.
    std::vector<header> headers;
    std::string body;
};

/// Status line and header fields of a response, as `parse_response_head`
/// reads them.
struct response_head
{
    std::string version; ///< For example "HTTP/1.1".
    int status = 0;
    std::string reason;
    std::vector<header> headers;
};

/// One event of a `text/event-stream` body (see `parse_sse_event`).
struct server_sent_event
{
    std::string type = "message"; ///< The `event:` field, or "message".
    std::string data;             ///< `data:` lines joined by newlines.
    std::string id;               ///< Last `id:` field without a NUL.
    std::optional<int> retry_ms;  ///< A valid `retry:` field, if any.
};

inline char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return c;
}

/// ASCII case-insensitive equality, as header names compare.
inline bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

/// Splits an `http://` or `https://` URL into host, port and target.
///
/// The authority ends at the first '/', and its last ':' separates the port,
/// which is not checked to be numeric. There is no support for userinfo,
/// bracketed IPv6 literals or fragments, and a query must follow a '/'
/// ("http://h/?q", not "http://h?q"). Throws `protocol_error` for another
/// scheme, an empty host or an empty port.
inline url parse_url(std::string_view text)
{
    auto tls = false;
    if (text.starts_with("http://")) {
        text.remove_prefix(std::string_view{"http://"}.size());
    } else if (text.starts_with("https://")) {
        text.remove_prefix(std::string_view{"https://"}.size());
        tls = true;
    } else {
        throw protocol_error{"only http:// and https:// URLs are supported"};
    }

    auto slash = text.find('/');
    auto authority = text.substr(0, slash);
    auto target = slash == std::string_view::npos ? std::string_view{"/"}
                                                  : text.substr(slash);
    if (authority.empty())
        throw protocol_error{"URL host is empty"};

    auto parsed = url{
        .tls = tls,
        .host = {},
        .port = tls ? "443" : "80",
        .target = std::string{target},
    };

    auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
        if (parsed.host.empty() || parsed.port.empty())
            throw protocol_error{"invalid URL authority"};
    } else {
        parsed.host = authority;
    }

    return parsed;
}

inline std::string_view trim_ascii(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

inline bool is_default_port(const url & parsed)
{
    return (!parsed.tls && parsed.port == "80")
        || (parsed.tls && parsed.port == "443");
}

/// The Host header value for PARSED: the host, plus ":port" unless the port
/// is the scheme's default.
inline std::string host_header(const url & parsed)
{
    if (is_default_port(parsed))
        return parsed.host;
    return parsed.host + ":" + parsed.port;
}

/// Encodes REQ as HTTP/1.1 request bytes, body included.
///
/// Adds `Content-Length` (the body size, also 0) unless REQ has one, and
/// `Connection: close` unless REQ sets Connection, so by default the
/// response ends the connection. Header names and values are written as
/// given: callers that take them from untrusted input must reject CR, LF
/// and other control characters themselves.
inline std::string serialize(const request & req)
{
    auto out = std::string{};
    out += req.method;
    out += ' ';
    out += req.target;
    out += " HTTP/1.1\r\n";

    if (!req.host.empty()) {
        out += "Host: ";
        out += req.host;
        out += "\r\n";
    }

    auto has_content_length = false;
    auto has_connection = false;
    for (auto const & h : req.headers) {
        has_content_length =
            has_content_length || iequals(h.name, "content-length");
        has_connection = has_connection || iequals(h.name, "connection");

        out += h.name;
        out += ": ";
        out += h.value;
        out += "\r\n";
    }

    if (!has_content_length) {
        out += "Content-Length: ";
        out += std::to_string(req.body.size());
        out += "\r\n";
    }

    if (!has_connection)
        out += "Connection: close\r\n";

    out += "\r\n";
    out += req.body;
    return out;
}

/// Parses a response status line and header fields (CRLF-separated, without
/// the blank line). Field values are trimmed; lines without ':' are
/// skipped. Throws `protocol_error` for a malformed status line.
inline response_head parse_response_head(std::span<const std::byte> bytes)
{
    auto text = as_string_view(bytes);
    auto head = response_head{};
    auto first = true;

    while (!text.empty()) {
        auto eol = text.find("\r\n");
        auto line =
            eol == std::string_view::npos ? text : text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{}
                                             : text.substr(eol + 2);

        if (first) {
            auto first_space = line.find(' ');
            if (first_space == std::string_view::npos)
                throw protocol_error{"malformed HTTP status line"};

            auto second_space = line.find(' ', first_space + 1);
            auto status_text = line.substr(
                first_space + 1,
                second_space == std::string_view::npos
                    ? std::string_view::npos
                    : second_space - first_space - 1);

            auto status = 0;
            auto [ptr, ec] = std::from_chars(
                status_text.data(),
                status_text.data() + status_text.size(),
                status,
                10);
            if (ec != std::errc{}
                || ptr != status_text.data() + status_text.size())
                throw protocol_error{"malformed HTTP status code"};

            head.version = line.substr(0, first_space);
            head.status = status;
            head.reason = second_space == std::string_view::npos
                            ? std::string{}
                            : std::string{line.substr(second_space + 1)};
            first = false;
            continue;
        }

        auto colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;

        head.headers.push_back(header{
            .name = std::string{trim_ascii(line.substr(0, colon))},
            .value = std::string{trim_ascii(line.substr(colon + 1))},
        });
    }

    if (head.version.empty())
        throw protocol_error{"missing response status line"};
    return head;
}

/// Reads one response head from READER, consuming through the blank line
/// and leaving the body in READER.
///
/// The whole head must fit in READER's buffer, which therefore bounds the
/// head size; a longer head fails with a buffer error, and end of input
/// before the blank line throws an end-of-stream error. Interim 1xx
/// responses are returned like any other head, not skipped.
inline task<response_head> read_response_head(bytefeed & reader)
{
    co_return parse_response_head(co_await reader.take_until("\r\n\r\n"));
}

/// The value of the first header named NAME (case-insensitive), borrowed
/// from RESPONSE.
inline std::optional<std::string_view>
header_value(const response_head & response, std::string_view name)
{
    for (const auto & h : response.headers) {
        if (iequals(h.name, name))
            return h.value;
    }
    return std::nullopt;
}

/// Whether the first NAME header's comma-separated list contains TOKEN,
/// compared case-insensitively.
inline bool has_header_token(
    const response_head & response,
    std::string_view name,
    std::string_view token)
{
    auto value = header_value(response, name);
    if (!value)
        return false;

    auto rest = *value;
    while (true) {
        auto comma = rest.find(',');
        auto part = trim_ascii(rest.substr(0, comma));
        if (iequals(part, token))
            return true;
        if (comma == std::string_view::npos)
            return false;
        rest.remove_prefix(comma + 1);
    }
}

/// The Content-Length, if present; throws `protocol_error` if it is not a
/// plain decimal number.
inline std::optional<std::size_t>
content_length(const response_head & response)
{
    auto value = header_value(response, "content-length");
    if (!value)
        return std::nullopt;

    auto parsed = std::size_t{0};
    auto [ptr, ec] = std::from_chars(
        value->data(), value->data() + value->size(), parsed, 10);
    if (ec != std::errc{} || ptr != value->data() + value->size())
        throw protocol_error{"invalid Content-Length"};
    return parsed;
}

inline bool is_chunked(const response_head & response)
{
    return has_header_token(response, "transfer-encoding", "chunked");
}

enum class content_encoding
{
    identity,
    gzip,
    deflate,
    zstd,
    brotli,
};

/// The response's single Content-Encoding. `identity` entries are ignored;
/// stacked encodings and unknown codings throw `protocol_error`.
inline content_encoding response_content_encoding(const response_head & response)
{
    auto value = header_value(response, "content-encoding");
    if (!value)
        return content_encoding::identity;

    auto encoding = content_encoding::identity;
    auto have_encoding = false;
    auto rest = *value;
    while (true) {
        auto comma = rest.find(',');
        auto part = trim_ascii(rest.substr(0, comma));
        if (!part.empty() && !iequals(part, "identity")) {
            if (have_encoding)
                throw protocol_error{
                    "stacked Content-Encoding values are not supported"};

            if (iequals(part, "gzip") || iequals(part, "x-gzip")) {
                encoding = content_encoding::gzip;
            } else if (iequals(part, "deflate")) {
                encoding = content_encoding::deflate;
            } else if (iequals(part, "zstd")) {
                encoding = content_encoding::zstd;
            } else if (iequals(part, "br")) {
                encoding = content_encoding::brotli;
            } else {
                throw protocol_error{"unsupported Content-Encoding"};
            }
            have_encoding = true;
        }

        if (comma == std::string_view::npos)
            return encoding;
        rest.remove_prefix(comma + 1);
    }
}

inline std::size_t parse_chunk_size(std::span<const std::byte> line)
{
    auto text = as_string_view(line);
    auto end = text.find(';');
    auto size_text = text.substr(0, end);
    if (size_text.empty())
        throw protocol_error{"empty chunk size"};

    auto size = std::size_t{0};
    auto [ptr, ec] = std::from_chars(
        size_text.data(), size_text.data() + size_text.size(), size, 16);
    if (ec != std::errc{} || ptr != size_text.data() + size_text.size())
        throw protocol_error{"invalid chunk size"};
    return size;
}

/// A byte feed of one response body with the transfer framing removed.
///
/// The framing comes from HEAD: chunked if Transfer-Encoding lists
/// "chunked", else Content-Length, else everything until READER ends. It
/// does not decode Content-Encoding (see `response_body_decoding_reader`)
/// and does not know about bodiless responses: for HEAD requests and
/// 1xx/204/304 statuses, do not read a body at all.
///
/// READER is borrowed and must outlive this reader; it should be the feed
/// that `read_response_head` consumed the head from. HEAD is only read
/// during construction. Truncated bodies, malformed chunk framing and
/// chunk trailers throw `protocol_error`. Reading stops at the end of this
/// body, so a kept-alive READER is left at the next response.
class http_body_reader final : public bytefeed
{
public:
    http_body_reader(
        bytefeed & reader,
        const response_head & head,
        std::size_t buffer_size = 4096)
        : bytefeed(buffer_size)
        , reader_(&reader)
    {
        configure(head);
    }

    http_body_reader(
        bytefeed & reader,
        const response_head & head,
        std::span<std::byte> buffer)
        : bytefeed(buffer)
        , reader_(&reader)
    {
        configure(head);
    }

    /// The next piece of body, at most LIMIT bytes, or nullopt at the end.
    ///
    /// The span borrows from the underlying reader's buffer and is valid
    /// until the next read from it. Bytes taken this way bypass this
    /// feed's own buffer; do not mix `next` with the bytefeed reads.
    task<std::optional<std::span<const std::byte>>>
    next(std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (done_)
            co_return std::nullopt;

        switch (mode_) {
        case mode::content_length:
            co_return co_await next_content_length(limit);
        case mode::chunked:
            co_return co_await next_chunked(limit);
        case mode::until_eof:
            co_return co_await next_until_eof(limit);
        }

        co_return std::nullopt;
    }

private:
    enum class mode
    {
        content_length,
        chunked,
        until_eof,
    };

    void configure(const response_head & head)
    {
        if (is_chunked(head)) {
            mode_ = mode::chunked;
        } else if (auto length = content_length(head)) {
            mode_ = mode::content_length;
            remaining_ = *length;
            done_ = remaining_ == 0;
        } else {
            mode_ = mode::until_eof;
        }
    }

    hope<fare_t> stream_more(
        bytesink & writer,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<fare_t>::ready(0);
        return stream_more_task(writer, limit);
    }

    task<fare_t> stream_more_task(
        bytesink & writer,
        std::size_t limit)
    {
        if (done_)
            co_return eof;

        if (auto dst = writer.unused_capacity(); !dst.empty())
            limit = std::min(limit, dst.size());

        auto chunk = co_await next(limit);
        if (!chunk)
            co_return eof;

        co_await nxtrt::write(writer, *chunk);

        if (done_ && chunk->empty())
            co_return eof;
        co_return chunk->size();
    }

    task<std::optional<std::span<const std::byte>>>
    next_content_length(std::size_t limit)
    {
        if (remaining_ == 0) {
            done_ = true;
            co_return std::nullopt;
        }

        auto chunk = co_await reader_->take_some(std::min(limit, remaining_));
        if (!chunk)
            throw protocol_error{"unexpected end of content-length body"};

        remaining_ -= chunk->size();
        if (remaining_ == 0)
            done_ = true;
        co_return chunk;
    }

    task<std::optional<std::span<const std::byte>>>
    next_chunked(std::size_t limit)
    {
        while (true) {
            if (remaining_ > 0) {
                auto chunk =
                    co_await reader_->take_some(std::min(limit, remaining_));
                if (!chunk)
                    throw protocol_error{"unexpected end of chunked body"};

                remaining_ -= chunk->size();
                if (remaining_ == 0 && !chunk->empty()) {
                    auto crlf = co_await reader_->take_until("\r\n");
                    if (!crlf.empty())
                        throw protocol_error{
                            "chunk data was not followed by CRLF"};
                }
                co_return chunk;
            }

            auto line = co_await reader_->take_until("\r\n");
            auto size = parse_chunk_size(line);
            if (size == 0) {
                auto trailers = co_await reader_->take_until("\r\n");
                if (!trailers.empty())
                    throw protocol_error{"chunk trailers are not supported"};
                done_ = true;
                co_return std::nullopt;
            }
            remaining_ = size;
        }
    }

    task<std::optional<std::span<const std::byte>>>
    next_until_eof(std::size_t limit)
    {
        auto chunk = co_await reader_->take_some(limit);
        if (!chunk)
            done_ = true;
        co_return chunk;
    }

    bytefeed * reader_;
    mode mode_ = mode::until_eof;
    std::size_t remaining_ = 0;
    bool done_ = false;
};

/// A byte feed of one response body, de-framed and decompressed.
///
/// Wraps an `http_body_reader` and, by the response's Content-Encoding, a
/// gzip, deflate (zlib-wrapped), zstd or brotli decoder; zstd and brotli
/// are available only when the build defines `NXTRT_HAVE_ZSTD` /
/// `NXTRT_HAVE_BROTLI`. Construction throws `protocol_error` for an
/// encoding it cannot decode. The same caveats as `http_body_reader`
/// apply: READER is borrowed and must outlive this reader, and bodiless
/// responses must not be read. The size limit for the decoded body is the
/// caller's to enforce.
///
/// @code
/// auto head = co_await http::read_response_head(transport);
/// auto body = http::response_body_decoding_reader{transport, head};
/// auto bytes = co_await body.take_some(4096); // nullopt at the end
/// @endcode
class response_body_decoding_reader final : public bytefeed
{
public:
    response_body_decoding_reader(
        bytefeed & reader,
        const response_head & head,
        std::size_t buffer_size = 4096)
        : bytefeed(buffer_size)
        , transfer_(reader, head, buffer_size)
        , active_(&transfer_)
    {
        configure(head, buffer_size);
    }

    response_body_decoding_reader(
        bytefeed & reader,
        const response_head & head,
        std::span<std::byte> buffer)
        : bytefeed(buffer)
        , transfer_(reader, head, buffer.size())
        , active_(&transfer_)
    {
        configure(head, buffer.size());
    }

    task<std::optional<std::span<const std::byte>>>
    next(std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        co_return co_await take_some(limit);
    }

private:
    void configure(const response_head & head, std::size_t buffer_size)
    {
        switch (response_content_encoding(head)) {
        case content_encoding::identity:
            break;
        case content_encoding::gzip:
            decoded_zlib_.emplace(transfer_, zlib_format::gzip, buffer_size);
            active_ = &*decoded_zlib_;
            break;
        case content_encoding::deflate:
            decoded_zlib_.emplace(transfer_, zlib_format::zlib, buffer_size);
            active_ = &*decoded_zlib_;
            break;
        case content_encoding::zstd:
#if defined(NXTRT_HAVE_ZSTD)
            decoded_zstd_.emplace(transfer_, buffer_size);
            active_ = &*decoded_zstd_;
#else
            throw protocol_error{
                "zstd Content-Encoding is not supported by this build"};
#endif
            break;
        case content_encoding::brotli:
#if defined(NXTRT_HAVE_BROTLI)
            decoded_brotli_.emplace(transfer_, buffer_size);
            active_ = &*decoded_brotli_;
#else
            throw protocol_error{
                "brotli Content-Encoding is not supported by this build"};
#endif
            break;
        }
    }

    hope<fare_t> stream_more(
        bytesink & writer,
        std::size_t limit) override
    {
        return active_->stream(writer, limit);
    }

    http_body_reader transfer_;
    std::optional<zlib_reader> decoded_zlib_;
#if defined(NXTRT_HAVE_ZSTD)
    std::optional<zstd_reader> decoded_zstd_;
#endif
#if defined(NXTRT_HAVE_BROTLI)
    std::optional<brotli_reader> decoded_brotli_;
#endif
    bytefeed * active_;
};

/// Reads the next server-sent event from READER, or nullopt at a clean end.
///
/// Follows the event-stream rules: LF or CRLF line ends, ':' comments,
/// `data` lines joined by newlines, and blocks without `data` skipped. A
/// final event without its blank line is still returned at end of input;
/// end of input in a block that has fields but no data throws
/// `protocol_error`.
inline task<std::optional<server_sent_event>>
parse_sse_event(bytefeed & reader)
{
    auto event = server_sent_event{};
    auto have_data = false;
    auto have_fields = false;

    while (true) {
        auto raw = std::span<const std::byte>{};
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
            have_fields = false;
            continue;
        }

        have_fields = true;

        auto text = as_string_view(raw);
        if (text.front() == ':')
            continue;

        auto colon = text.find(':');
        auto field = colon == std::string_view::npos ? text
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

/// A `feed` of server-sent events parsed from a borrowed byte feed (usually
/// a decoded response body), which must outlive it.
class sse_feed final : public feed<server_sent_event>
{
public:
    explicit sse_feed(bytefeed & reader, std::size_t buffer_size = 1)
        : feed<server_sent_event>(buffer_size)
        , reader_(&reader)
    {}

    sse_feed(bytefeed & reader, value_storage_ref<server_sent_event> buffer)
        : feed<server_sent_event>(buffer)
        , reader_(&reader)
    {}

private:
    task<std::optional<server_sent_event>> next_value() override
    {
        co_return co_await parse_sse_event(*reader_);
    }

    bytefeed * reader_;
};

/// Makes an `sse_feed` over READER, buffering BUFFER_SIZE events.
inline sse_feed sse_event_parser(
    bytefeed & reader,
    std::size_t buffer_size = 1)
{
    return sse_feed{
        reader,
        buffer_size,
    };
}

inline sse_feed sse_event_parser(
    bytefeed & reader,
    value_storage_ref<server_sent_event> buffer)
{
    return sse_feed{
        reader,
        buffer,
    };
}

} // namespace nxtrt::http
