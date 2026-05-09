#include <nxtio/http.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace nxt::io::http {

namespace {

char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return c;
}

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
           && std::ranges::equal(a, b, {}, ascii_lower, ascii_lower);
}

std::string_view trim_ascii(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

} // namespace

url parse_url(std::string_view text)
{
    auto tls = false;
    if (text.starts_with("http://")) {
        text.remove_prefix(std::string_view{"http://"}.size());
    } else if (text.starts_with("https://")) {
        text.remove_prefix(std::string_view{"https://"}.size());
        tls = true;
    } else {
        throw protocol_error{
            "only http:// and https:// URLs are supported"};
    }

    auto slash = text.find('/');
    auto authority = text.substr(0, slash);
    auto target = slash == std::string_view::npos ? std::string_view{"/"}
                                                  : text.substr(slash);
    if (authority.empty())
        throw protocol_error{"URL host is empty"};

    url parsed;
    parsed.tls = tls;
    parsed.port = tls ? "443" : "80";
    parsed.target = target;

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

std::uint16_t parse_port(std::string_view text)
{
    auto port = unsigned{0};
    auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), port, 10);
    if (ec != std::errc{} || ptr != text.data() + text.size()
        || port > 65535)
        throw protocol_error{"invalid URL port"};
    return static_cast<std::uint16_t>(port);
}

bool is_default_port(const url & url)
{
    return (!url.tls && url.port == "80") || (url.tls && url.port == "443");
}

nxt::http::response_head parse_response_head(std::span<const std::byte> bytes)
{
    auto text = as_text(bytes);
    auto head = nxt::http::response_head{};
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

        head.headers.push_back(nxt::http::header{
            .name = std::string{trim_ascii(line.substr(0, colon))},
            .value = std::string{trim_ascii(line.substr(colon + 1))},
        });
    }

    if (head.version.empty())
        throw protocol_error{"missing response status line"};

    return head;
}

std::string_view as_text(std::span<const std::byte> bytes)
{
    return std::string_view{
        reinterpret_cast<const char *>(bytes.data()), bytes.size_bytes()};
}

std::optional<std::string_view>
header_value(const nxt::http::response_head & response, std::string_view name)
{
    for (const auto & h : response.headers) {
        if (iequals(h.name, name))
            return h.value;
    }
    return std::nullopt;
}

bool has_header_token(
    const nxt::http::response_head & response,
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

std::optional<std::size_t>
response_content_length(const nxt::http::response_head & response)
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

bool response_is_chunked(const nxt::http::response_head & response)
{
    return has_header_token(response, "transfer-encoding", "chunked");
}

bool response_status_is_success(const nxt::http::response_head & response)
{
    return response.status >= 200 && response.status < 300;
}

bool response_content_type_is(
    const nxt::http::response_head & response,
    std::string_view expected)
{
    auto value = header_value(response, "content-type");
    if (!value)
        return false;

    auto media_type = trim_ascii(value->substr(0, value->find(';')));
    return iequals(media_type, expected);
}

std::string response_status_text(const nxt::http::response_head & response)
{
    auto text = response.version + " " + std::to_string(response.status);
    if (!response.reason.empty()) {
        text += " ";
        text += response.reason;
    }
    return text;
}

std::size_t parse_chunk_size(std::span<const std::byte> line)
{
    auto text = as_text(line);
    auto end = text.find(';');
    auto size_text = text.substr(0, end);
    if (size_text.empty())
        throw protocol_error{"empty chunk size"};

    auto size = std::size_t{0};
    auto * first = size_text.data();
    auto * last = first + size_text.size();
    auto [ptr, ec] = std::from_chars(first, last, size, 16);
    if (ec != std::errc{} || ptr != last)
        throw protocol_error{"invalid chunk size"};

    return size;
}

} // namespace nxt::io::http
