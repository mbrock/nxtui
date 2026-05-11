#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::http {

/// HTTP header name/value pair.
struct header
{
    /// Header field name.
    std::string name;
    /// Header field value.
    std::string value;
};

/// Parsed HTTP response start line and headers.
struct response_head
{
    /// HTTP version string, such as `HTTP/1.1`.
    std::string version;
    /// Numeric status code.
    int status = 0;
    /// Reason phrase.
    std::string reason;
    /// Response headers in wire order.
    std::vector<header> headers;
};

/// Minimal HTTP request representation.
struct request
{
    /// Request method.
    std::string method = "GET";
    /// Request target path and query.
    std::string target = "/";
    /// Host header value.
    std::string host;
    /// Extra request headers.
    std::vector<header> headers;
    /// Request body bytes.
    std::string body;
};

/// Parsed server-sent event.
struct server_sent_event
{
    /// Event type; defaults to `message`.
    std::string type = "message";
    /// Event data payload.
    std::string data;
    /// Last event id.
    std::string id;
    /// Optional retry delay in milliseconds.
    std::optional<int> retry_ms;
};

/// Error raised for malformed HTTP or SSE data.
struct parse_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Incremental parser for server-sent event text.
class server_sent_event_parser
{
public:
    /// Feed a body chunk and return complete events.
    [[nodiscard]] std::vector<server_sent_event> feed(std::string_view body)
    {
        std::vector<server_sent_event> events;
        for (auto c : body) {
            if (c == '\r') {
                process_line(events, line_);
                line_.clear();
                previous_was_cr_ = true;
                continue;
            }

            if (c == '\n') {
                if (previous_was_cr_) {
                    previous_was_cr_ = false;
                    continue;
                }

                process_line(events, line_);
                line_.clear();
                continue;
            }

            previous_was_cr_ = false;
            line_ += c;
        }

        return events;
    }

    /// Finish the stream and return any final pending event.
    [[nodiscard]] std::vector<server_sent_event> close()
    {
        std::vector<server_sent_event> events;
        previous_was_cr_ = false;
        if (!line_.empty()) {
            process_line(events, line_);
            line_.clear();
        }
        dispatch(events);
        return events;
    }

private:
    void
    process_line(std::vector<server_sent_event> & events, std::string_view line)
    {
        if (line.empty()) {
            dispatch(events);
            return;
        }

        if (line.front() == ':')
            return;

        auto colon = line.find(':');
        auto field = colon == std::string_view::npos ? line
                                                     : line.substr(0, colon);
        auto value = colon == std::string_view::npos
                         ? std::string_view{}
                         : line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
            value.remove_prefix(1);

        if (field == "data") {
            pending_.data += value;
            pending_.data += '\n';
        } else if (field == "event") {
            pending_.type = value.empty() ? "message" : std::string{value};
        } else if (field == "id") {
            if (value.find('\0') == std::string_view::npos) {
                pending_.id = value;
                pending_has_id_ = true;
            }
        } else if (field == "retry") {
            auto retry = 0;
            auto * first = value.data();
            auto * last = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(first, last, retry);
            if (!value.empty() && ec == std::errc{} && ptr == last)
                pending_.retry_ms = retry;
        }
    }

    void dispatch(std::vector<server_sent_event> & events)
    {
        if (pending_has_id_)
            last_id_ = pending_.id;

        if (!pending_.data.empty()) {
            if (pending_.data.back() == '\n')
                pending_.data.pop_back();

            if (!pending_has_id_)
                pending_.id = last_id_;

            events.push_back(pending_);
        }

        pending_ = server_sent_event{};
        pending_has_id_ = false;
    }

    std::string line_;
    bool previous_was_cr_ = false;
    server_sent_event pending_;
    bool pending_has_id_ = false;
    std::string last_id_;
};

/// Serialize a request to HTTP/1.1 bytes.
[[nodiscard]] inline std::string serialize(const request & req)
{
    std::string out;
    out += req.method;
    out += ' ';
    out += req.target;
    out += " HTTP/1.1\r\n";

    if (!req.host.empty()) {
        out += "Host: ";
        out += req.host;
        out += "\r\n";
    }

    bool has_content_length = false;
    bool has_connection = false;
    auto header_name_is = [](std::string_view lhs, std::string_view rhs) {
        return lhs.size() == rhs.size()
            && std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](
                             char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a))
                       == std::tolower(static_cast<unsigned char>(b));
               });
    };

    for (const auto & h : req.headers) {
        has_content_length =
            has_content_length || header_name_is(h.name, "content-length");
        has_connection =
            has_connection || header_name_is(h.name, "connection");

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

} // namespace nxt::http
