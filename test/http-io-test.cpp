#include <nxtio/http.hpp>

#include <boost/ut.hpp>

#include <array>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nxtio/async.hpp>

namespace nxt::test {

using namespace boost::ut;

suite http_io_tests = [] {
    namespace io_http = nxt::io::http;
    using namespace std::literals;

    "url parser recognizes http and https defaults"_test = [] {
        auto http_url = io_http::parse_url("http://example.test/path"sv);
        expect(!http_url.tls);
        expect(http_url.host == "example.test");
        expect(http_url.port == "80");
        expect(http_url.target == "/path");

        auto https_url = io_http::parse_url("https://example.test:8443"sv);
        expect(https_url.tls);
        expect(https_url.host == "example.test");
        expect(https_url.port == "8443");
        expect(https_url.target == "/");
    };

    "response head parser detects length and chunked bodies"_test = [] {
        auto fixed = io_http::parse_response_head(
            std::as_bytes(
                std::span{"HTTP/1.1 200 OK\r\nContent-Length: 11\r\n"sv}));
        expect(io_http::response_status_text(fixed) == "HTTP/1.1 200 OK");
        expect(io_http::response_content_length(fixed) == 11_ul);
        expect(!io_http::response_is_chunked(fixed));

        auto chunked = io_http::parse_response_head(
            std::as_bytes(
                std::span{
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n"sv}));
        expect(io_http::response_status_text(chunked) == "HTTP/1.1 200 OK");
        expect(!io_http::response_content_length(chunked));
        expect(io_http::response_is_chunked(chunked));
    };

    "byte cursor parses complete in-memory slices"_test = [] {
        nxt::io::byte_cursor input(
            "HTTP/1.1 200 Hello\r\n"
            "Server: Comanche\r\n"
            "Content-Type: text/slop\r\n"
            "\r\n"
            "hello"sv);

        auto head = input.take_until("\r\n\r\n");
        std::vector<std::string> headers;
        for (auto header : nxt::io::as_string_view(head)
                               | std::views::split("\r\n"sv))
            headers.emplace_back(header.begin(), header.end());

        expect(headers.size() == 3_ul);
        expect(headers[0] == "HTTP/1.1 200 Hello");
        expect(headers[1] == "Server: Comanche");
        expect(headers[2] == "Content-Type: text/slop");
        expect(nxt::io::as_string_view(input.remaining()) == "hello");
    };

    "byte reader refills until the slurp cut appears"_test = [] {
        auto chunks = std::array{
            "HTTP/1.1 200 Hello\r\nSer"sv,
            "ver: Comanche\r\n\r\nhello"sv,
        };
        nxt::io::string_transport transport{std::span{chunks}};
        std::array<std::byte, 4096> buffer{};
        auto reader = nxt::io::byte_reader{transport, std::span{buffer}};

        auto head = nxt::sync_wait(reader.take_until("\r\n\r\n"));

        auto text = io_http::as_text(head);
        expect(text == "HTTP/1.1 200 Hello\r\nServer: Comanche");
        expect(io_http::as_text(reader.buffered()) == "hello");
    };

    "content-length reader yields buffered read-ahead then refill chunks"_test = [] {
        auto chunks = std::array{
            "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhe"sv,
            "llo world"sv,
        };
        nxt::io::string_transport transport{std::span{chunks}};
        std::array<std::byte, 4096> buffer{};
        auto reader = nxt::io::byte_reader{transport, std::span{buffer}};

        auto head = nxt::sync_wait(reader.take_until("\r\n\r\n"));

        expect(
            io_http::as_text(head)
            == "HTTP/1.1 200 OK\r\nContent-Length: 11");
        expect(io_http::as_text(reader.buffered()) == "hello world");

        std::string body;
        auto on_chunk =
            [&](std::span<const std::byte> chunk) -> nxt::task<> {
            body += io_http::as_text(chunk);
            co_return;
        };

        nxt::sync_wait(
            io_http::read_content_length(
                reader, 11, on_chunk));

        expect(body == "hello world");
    };

    "chunked reader yields chunks across read-ahead and refills"_test = [] {
        auto chunks = std::array{
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhe"sv,
            "llo\r\n6\r\n world\r\n0\r\n\r\n"sv,
        };
        nxt::io::string_transport transport{std::span{chunks}};
        std::array<std::byte, 4096> buffer{};
        auto reader = nxt::io::byte_reader{transport, std::span{buffer}};

        auto head = nxt::sync_wait(reader.take_until("\r\n\r\n"));

        expect(
            io_http::as_text(head)
            == "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
        expect(
            io_http::as_text(reader.buffered())
            == "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");

        std::string body;
        auto on_chunk =
            [&](std::span<const std::byte> chunk) -> nxt::task<> {
            body += io_http::as_text(chunk);
            co_return;
        };

        nxt::sync_wait(
            io_http::read_chunked(reader, on_chunk));

        expect(body == "hello world");
    };

    "send request returns head before streaming sse body"_test = [] {
        auto sse =
            "event: greeting\n"
            "data: hello\n"
            "\n"s;
        auto response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: "
            + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        auto chunks = std::array{std::string_view{response}};
        nxt::io::string_transport transport{std::span{chunks}};
        std::array<std::byte, 4096> buffer{};
        auto reader = nxt::io::byte_reader{transport, std::span{buffer}};

        auto head = nxt::sync_wait(
            io_http::send_request(
                transport,
                reader,
                nxt::http::request{
                    .method = "POST",
                    .target = "/stream",
                    .host = "example.test",
                    .headers = {{"Accept", "text/event-stream"}},
                    .body = "{}",
                }));

        expect(head.head.status == 200_i);
        expect(io_http::response_content_type_is(
            head.head, "text/event-stream"));

        std::vector<nxt::http::server_sent_event> events;
        auto on_event = [&](nxt::http::server_sent_event event) -> nxt::task<> {
            events.push_back(std::move(event));
            co_return;
        };

        nxt::sync_wait(io_http::read_sse_response(reader, head, on_event));

        expect(transport.written().starts_with("POST /stream HTTP/1.1\r\n"));
        expect(events.size() == 1_ul);
        expect(events[0].type == "greeting");
        expect(events[0].data == "hello");
    };

    "response text reader collects non-streaming error bodies"_test = [] {
        auto response =
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\n\r\nbad request"s;
        auto chunks = std::array{std::string_view{response}};
        nxt::io::string_transport transport{std::span{chunks}};
        std::array<std::byte, 4096> buffer{};
        auto reader = nxt::io::byte_reader{transport, std::span{buffer}};

        auto head = nxt::sync_wait(
            io_http::send_request(
                transport,
                reader,
                nxt::http::request{
                    .target = "/fail",
                    .host = "example.test",
                    .headers = {},
                    .body = {},
                }));

        expect(head.head.status == 400_i);
        auto body = nxt::sync_wait(io_http::read_response_text(reader, head));

        expect(body == "bad request");
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
