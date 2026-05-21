#pragma once

#include "nxt/rt/buffers.hpp"
#include "nxt/rt/pipe.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::rt::http {

struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct header
{
    std::string name;
    std::string value;
};

struct response_head
{
    std::string version;
    int status = 0;
    std::string reason;
    std::vector<header> headers;
};

inline char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return c;
}

inline bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

inline std::string_view trim_ascii(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

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

template<typename Reader>
task<response_head> read_response_head(Reader & reader)
{
    co_return parse_response_head(co_await reader.take_until("\r\n\r\n"));
}

inline std::optional<std::string_view>
header_value(const response_head & response, std::string_view name)
{
    for (const auto & h : response.headers) {
        if (iequals(h.name, name))
            return h.value;
    }
    return std::nullopt;
}

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

template<typename Reader>
pipe<std::span<const std::byte>>
read_content_length(Reader & reader, std::size_t length)
{
    auto remaining = length;
    while (remaining > 0) {
        auto available = reader.buffered();
        if (available.empty()) {
            if (co_await reader.fill_more() == 0)
                throw protocol_error{"unexpected end of content-length body"};
            available = reader.buffered();
        }

        auto n = std::min(remaining, available.size());
        auto chunk = available.first(n);
        reader.toss(n);
        remaining -= n;
        co_yield chunk;
    }
}

template<typename Reader>
pipe<std::span<const std::byte>> read_chunked(Reader & reader)
{
    while (true) {
        auto line = co_await reader.take_until("\r\n");
        auto size = parse_chunk_size(line);
        if (size == 0) {
            auto trailers = co_await reader.take_until("\r\n");
            if (!trailers.empty())
                throw protocol_error{"chunk trailers are not supported"};
            co_return;
        }

        auto body = read_content_length(reader, size);
        while (auto chunk = co_await body.next())
            co_yield *chunk;

        auto crlf = co_await reader.take_until("\r\n");
        if (!crlf.empty())
            throw protocol_error{"chunk data was not followed by CRLF"};
    }
}

template<typename Reader>
pipe<std::span<const std::byte>>
read_response_body(Reader & reader, const response_head & head)
{
    if (is_chunked(head)) {
        auto body = read_chunked(reader);
        while (auto chunk = co_await body.next())
            co_yield *chunk;
        co_return;
    }

    if (auto length = content_length(head)) {
        auto body = read_content_length(reader, *length);
        while (auto chunk = co_await body.next())
            co_yield *chunk;
        co_return;
    }

    while (true) {
        auto available = reader.buffered();
        if (available.empty()) {
            if (co_await reader.fill_more() == 0)
                co_return;
            available = reader.buffered();
        }

        reader.toss(available.size());
        co_yield available;
    }
}

} // namespace nxt::rt::http
