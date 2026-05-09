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

struct request
{
    std::string method = "GET";
    std::string target = "/";
    std::string host;
    std::vector<header> headers;
    std::string body;
};

struct server_sent_event
{
    std::string type = "message";
    std::string data;
    std::string id;
    std::optional<int> retry_ms;
};

struct parse_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class server_sent_event_parser
{
public:
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
