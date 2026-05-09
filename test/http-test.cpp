#include <nxt/http.hpp>

#include <boost/ut.hpp>

#include <string>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

suite http_tests = [] {
    "serializes minimal request"_test = [] {
        http::request req{
            .method = "POST",
            .target = "/v1/chat/completions",
            .host = "api.example.test",
            .headers = {
                {"Authorization", "Bearer token"},
                {"Content-Type", "application/json"},
            },
            .body = R"({"stream":true})",
        };

        auto wire = http::serialize(req);
        expect(wire == std::string{
                           "POST /v1/chat/completions HTTP/1.1\r\n"
                           "Host: api.example.test\r\n"
                           "Authorization: Bearer token\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: 15\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           R"({"stream":true})"});
    };

    "parses sse event fields across arbitrary boundaries"_test = [] {
        http::server_sent_event_parser parser;
        std::vector<http::server_sent_event> events;

        for (auto chunk : {
                 ": keepalive\n",
                 "event: build\nid: 7\ndata: start",
                 "\ndata: line two\n\n",
                 "data: done\n\n",
             }) {
            auto next = parser.feed(chunk);
            events.insert(events.end(), next.begin(), next.end());
        }

        expect(events.size() == 2_ul);
        expect(events[0].type == "build");
        expect(events[0].id == "7");
        expect(events[0].data == "start\nline two");
        expect(events[1].type == "message");
        expect(events[1].id == "7");
        expect(events[1].data == "done");
    };

    "flushes final sse event on close"_test = [] {
        http::server_sent_event_parser parser;
        auto events = parser.feed("data: final");
        expect(events.empty());

        events = parser.close();
        expect(events.size() == 1_ul);
        expect(events[0].data == "final");
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
