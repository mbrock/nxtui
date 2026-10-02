#include <nxtrt/http-server.hpp>
#include <nxtrt/net.hpp>
#include <nxtrt/bell.hpp>

#if defined(__linux__)
#  include <nxtrt/wand/epoll.hpp>
#  include <nxtrt/wand/uring.hpp>
#else
#  include <nxtrt/wand/kqueue.hpp>
#endif

#include "test.hpp"

#include <array>
#include <filesystem>

namespace nxt::test {
namespace {

using namespace boost::ut;
using namespace std::chrono_literals;
namespace http = nxtrt::http;
using nxtrt::task;

struct handler_state
{
    std::vector<http::request> requests;
    int active = 0;
    int peak = 0;
    nxtrt::bell entered;
    nxtrt::bell release;
};

task<http::response> handle(handler_state & state, http::request req)
{
    ++state.active;
    state.peak = std::max(state.peak, state.active);

    struct finished
    {
        handler_state & state;

        ~finished()
        {
            --state.active;
        }
    } guard{state};

    state.requests.push_back(req);
    state.entered.ring();
    if (req.target == "/slow")
        co_await nxtrt::op::timeout::after(5s);
    if (req.target == "/wait")
        co_await state.release;
    if (req.target == "/throw")
        throw std::runtime_error{"handler failed"};
    if (req.target == "/cancel")
        throw nxtrt::operation_cancelled{};
    auto res = http::response{};
    res.body = req.target + ":" + req.body;
    if (req.target == "/large")
        res.body.assign(8 * 1024 * 1024, 'z');
    if (req.target == "/204" || req.target == "/205"
        || req.target == "/304")
        res.status = std::stoi(req.target.substr(1));
    if (req.target == "/101")
        res.status = 101;
    if (req.target == "/bad-value")
        res.headers.push_back({"X-Test", "ok\r\nInjected: yes"});
    if (req.target == "/bad-name")
        res.headers.push_back({"Bad Name", "value"});
    if (req.target == "/nul")
        res.headers.push_back({"X-Test", std::string{"a\0b", 3}});
    if (req.target == "/managed") {
        res.headers = {
            {"Content-Length", "999"},
            {"Connection", "close"},
            {"Transfer-Encoding", "chunked"},
            {"Upgrade", "h2c"},
            {"X-Test", "yes"}};
    }
    co_return res;
}

template<class Wand>
struct server_fixture
{
    handler_state state;
    nxt::unique_fd listener = nxtrt::net::listen_tcp_loopback();
    sockaddr_in address = nxtrt::net::socket_address(listener.get());
    Wand wand;
    nxtrt::deck deck{&wand};
    nxtrt::root_task<void> server;
    bool stopped = false;

    explicit server_fixture(http::server_options options = {})
        : server(deck, [this, options] {
            return http::serve(
                listener.get(),
                [this](http::request req) {
                    return handle(state, std::move(req));
                },
                options);
        })
    {
        server.start();
    }

    ~server_fixture()
    {
        if (!stopped) {
            server.inner().request_stop();
            wand.run_until_done(deck, server.inner());
        }
    }

    template<class Fn, class... Args>
    void run(Fn fn, Args &&... args)
    {
        auto client = nxtrt::root_task{
            deck, [&] { return fn(std::forward<Args>(args)...); }};
        client.start();
        wand.run_until_done(deck, client.inner());
        std::move(client.inner()).result();
    }

    void stop()
    {
        server.inner().request_stop();
        wand.run_until_done(deck, server.inner());
        stopped = true;
        std::move(server.inner()).result();
        expect(state.active == 0);
        expect(::fcntl(listener.get(), F_GETFD) >= 0);
    }
};

struct client_socket
{
    std::array<std::byte, 4096> tx{}, rx{};
    nxtrt::net::socket socket;

    explicit client_socket(nxt::unique_fd fd)
        : socket(std::move(fd), tx, rx)
    {
    }
};

struct reply
{
    http::response_head head;
    std::string body;
};

task<reply> read_reply(client_socket & client, bool head_only = false)
{
    auto result = reply{};
    result.head = co_await http::read_response_head(client.socket.input());
    auto size =
        head_only ? 0 : http::content_length(result.head).value_or(0);
    while (size) {
        auto bytes = co_await client.socket.input().take_some(size);
        if (!bytes)
            throw std::runtime_error{"truncated HTTP response"};
        result.body.append(nxtrt::as_string_view(*bytes));
        size -= bytes->size();
    }
    co_return result;
}

task<void> send(client_socket & client, std::string_view bytes)
{
    co_await nxtrt::write_all(client.socket.output(), bytes);
}

std::string get(std::string_view target, bool close = true)
{
    return "GET " + std::string{target}
           + " HTTP/1.1\r\nHost: example.test\r\n"
           + (close ? "Connection: close\r\n\r\n" : "\r\n");
}

task<void> expect_closed(client_socket & client)
{
    expect(!(co_await client.socket.input().take_some()));
}

task<void> exchange(
    sockaddr_in address,
    std::string wire,
    int status,
    std::string body = {})
{
    auto client = client_socket{co_await nxtrt::net::connect(address)};
    co_await send(client, wire);
    auto res = co_await read_reply(client);
    expect(res.head.status == status);
    expect(res.body == body);
    expect(http::has_header_token(res.head, "Connection", "close"));
    co_await expect_closed(client);
}

task<void> fragmented_pipeline(sockaddr_in address)
{
    auto client = client_socket{co_await nxtrt::net::connect(address)};
    auto first = std::string{
        "POST /one HTTP/1.1\r\nHost: backend\r\n"
        "Forwarded: host=untrusted\r\nContent-Length: 5\r\n\r\n"};
    first += std::string{"a\0b\xffz", 5};
    for (auto c : first) {
        co_await send(client, std::string_view{&c, 1});
        co_await nxtrt::op::timeout::after(20us);
    }
    auto second = std::string{
        "POST /two HTTP/1.1\r\nHost: backend\r\n"
        "Transfer-Encoding: chunked\r\nTrailer: ETag\r\n\r\n"
        "2;abc=xyz\r\nxy\r\n3\r\n"};
    second += std::string{"\0qz", 3};
    second += "\r\n0\r\nETag: ignored\r\nHost: evil\r\n\r\n";
    // Split the chunk size/CRLF and pipeline a third request in the last
    // write.
    auto split = second.find("2;abc") + 1;
    co_await send(client, std::string_view{second}.substr(0, split));
    co_await nxtrt::op::timeout::after(20us);
    co_await send(client, second.substr(split) + get("/last"));
    auto one = co_await read_reply(client);
    auto two = co_await read_reply(client);
    auto three = co_await read_reply(client);
    expect(
        one.head.status == 200
        && one.body == std::string{"/one:a\0b\xffz", 10});
    expect(
        two.head.status == 200
        && two.body == std::string{"/two:xy\0qz", 10});
    expect(three.head.status == 200 && three.body == "/last:");
    co_await expect_closed(client);
}

task<void> head_and_no_body(sockaddr_in address)
{
    auto client = client_socket{co_await nxtrt::net::connect(address)};
    co_await send(
        client,
        "HEAD /head HTTP/1.1\r\nHost: h\r\n\r\n" + get("/204", false)
            + get("/304", false) + get("/205", false)
            + get("/managed", false) + get("/tail"));
    auto head = co_await read_reply(client, true);
    expect(head.head.status == 200);
    expect(http::content_length(head.head) == 6);
    for (auto status : {204, 304, 205}) {
        auto res = co_await read_reply(client);
        expect(res.head.status == status && res.body.empty());
        expect(
            http::content_length(res.head)
            == (status == 205 ? std::optional<std::size_t>{0}
                              : std::nullopt));
    }
    auto managed = co_await read_reply(client);
    expect(managed.body == "/managed:");
    expect(!http::header_value(managed.head, "Transfer-Encoding"));
    expect(!http::header_value(managed.head, "Upgrade"));
    expect(!http::has_header_token(managed.head, "Connection", "close"));
    expect(http::header_value(managed.head, "X-Test") == "yes");
    expect((co_await read_reply(client)).body == "/tail:");
    co_await expect_closed(client);
}

task<void> large_binary_body(sockaddr_in address)
{
    auto client = client_socket{co_await nxtrt::net::connect(address)};
    auto body = std::string(25001, '\0');
    for (std::size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<char>(i % 256);
    co_await send(
        client,
        "POST /binary HTTP/1.1\r\nHost: h\r\n"
        "Content-Length: 25001\r\nConnection: close\r\n\r\n");
    for (std::size_t i = 0; i < body.size(); i += 317)
        co_await send(client, std::string_view{body}.substr(i, 317));
    auto res = co_await read_reply(client);
    expect(res.head.status == 200 && res.body == "/binary:" + body);
    co_await expect_closed(client);
}

task<void> truncated_body(sockaddr_in address)
{
    auto client = client_socket{co_await nxtrt::net::connect(address)};
    co_await send(
        client,
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\nabc");
    ::shutdown(client.socket.fd(), SHUT_WR);
    expect((co_await read_reply(client)).head.status == 400);
    co_await expect_closed(client);
}

task<void> limited_pipeline(sockaddr_in address)
{
    auto client = client_socket{co_await nxtrt::net::connect(address)};
    co_await send(client, get("/a", false) + get("/b", false) + get("/c"));
    expect((co_await read_reply(client)).body == "/a:");
    auto last = co_await read_reply(client);
    expect(last.body == "/b:");
    expect(http::has_header_token(last.head, "Connection", "close"));
    co_await expect_closed(client);
}

task<void> overlap(sockaddr_in address, handler_state & state)
{
    auto a = client_socket{co_await nxtrt::net::connect(address)};
    co_await send(a, get("/wait"));
    co_await state.entered;
    state.entered.reset();
    auto b = client_socket{co_await nxtrt::net::connect(address)};
    co_await send(b, get("/b"));
    expect((co_await read_reply(b)).body == "/b:");
    expect(state.active == 1 && state.peak == 2);
    state.release.ring();
    expect((co_await read_reply(a)).body == "/wait:");
}

task<void> connection_limit(sockaddr_in address, handler_state & state)
{
    auto a = client_socket{co_await nxtrt::net::connect(address)};
    co_await send(a, get("/wait"));
    co_await state.entered;
    auto b = client_socket{co_await nxtrt::net::connect(address)};
    co_await send(b, get("/b"));
    co_await nxtrt::op::timeout::after(2ms);
    expect(state.requests.size() == 1);
    state.release.ring();
    expect((co_await read_reply(a)).body == "/wait:");
    expect((co_await read_reply(b)).body == "/b:");
    expect(state.peak == 1);
}

task<void> idle_timeout(sockaddr_in address)
{
    auto client = client_socket{co_await nxtrt::net::connect(address)};
    expect((co_await read_reply(client)).head.status == 408);
    co_await expect_closed(client);
}

task<void> stalled_writer(sockaddr_in address, handler_state & state)
{
    auto client = client_socket{co_await nxtrt::net::connect(address)};
    auto size = 1024;
    ::setsockopt(
        client.socket.fd(), SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    co_await send(client, get("/large"));
    co_await state.entered;
    // The sole worker cannot accept /after until the blocked write is
    // drained.
    co_await exchange(address, get("/after"), 200, "/after:");
}

task<void> disconnect_then_recover(sockaddr_in address)
{
    for (int i = 0; i < 8; ++i) {
        auto client = client_socket{co_await nxtrt::net::connect(address)};
        co_await send(
            client,
            i % 2 ? "POST / HTTP/1.1\r\nHost: h\r\n"
                    "Content-Length: 9\r\n\r\na"
                  : get("/large"));
        auto reset = linger{1, 0};
        ::setsockopt(
            client.socket.fd(),
            SOL_SOCKET,
            SO_LINGER,
            &reset,
            sizeof(reset));
    }
    co_await exchange(address, get("/after"), 200, "/after:");
}

task<void> park_connections(
    sockaddr_in address,
    std::vector<nxt::unique_fd> & peers,
    handler_state & state)
{
    for (auto wire :
         {std::string{"GET / HTTP/1.1\r\nHost: "},
          std::string{
              "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\na"},
          get("/slow")}) {
        auto fd = co_await nxtrt::net::connect(address);
        auto sink = nxtrt::socket_sink{fd.get()};
        co_await nxtrt::write_all(sink, wire);
        peers.push_back(std::move(fd));
    }
    co_await state.entered;
}

task<void> park_write(sockaddr_in address, nxt::unique_fd & peer)
{
    peer = co_await nxtrt::net::connect(address);
    auto sink = nxtrt::socket_sink{peer.get()};
    co_await nxtrt::write_all(sink, get("/large"));
    // Do not prefetch and discard bytes needed by partial_response_closed.
    auto source = nxtrt::socket_source{peer.get(), 0, 1};
    // Synchronize on the first response byte, not a wall-clock guess.
    // Leave the rest of the 8 MiB response unread until after cancellation.
    expect(bool(co_await source.take_some(1)));
}

task<void> partial_response_closed(int peer)
{
    auto source = nxtrt::socket_source{peer};
    auto total = std::size_t{};
    while (auto bytes = co_await source.take_some())
        total += bytes->size();
    expect(total > 0 && total < 8 * 1024 * 1024);
}

task<void> peers_closed(std::vector<nxt::unique_fd> & peers)
{
    for (auto & fd : peers) {
        auto source = nxtrt::socket_source{fd.get()};
        expect(!(co_await source.take_some()));
    }
}

task<void>
repeated_requests(sockaddr_in address, std::size_t count, bool reconnect)
{
    auto client = std::unique_ptr<client_socket>{};
    for (std::size_t i = 0; i < count; ++i) {
        if (!client)
            client = std::make_unique<client_socket>(
                co_await nxtrt::net::connect(address));
        auto target = "/request/" + std::to_string(i);
        co_await send(*client, get(target, reconnect || i + 1 == count));
        auto res = co_await read_reply(*client);
        expect(res.head.status == 200 && res.body == target + ":");
        if (reconnect)
            client.reset();
    }
}

template<class Wand>
void server_tests()
{
    "fragmented binary bodies and sequential pipelines"_test = [] {
        auto server = server_fixture<Wand>{};
        server.run(fragmented_pipeline, server.address);
        server.stop();
        expect(server.state.requests.size() == 3);
        expect(server.state.requests[0].host == "backend");
        expect(server.state.requests[1].host == "backend");
        for (auto const & h : server.state.requests[1].headers)
            expect(!http::iequals(h.name, "ETag"));
    };
    "HEAD and bodyless statuses do not desynchronize pipeline"_test = [] {
        auto server = server_fixture<Wand>{};
        server.run(head_and_no_body, server.address);
        server.stop();
    };
    "binary body spans many receive and send buffers"_test = [] {
        auto server = server_fixture<Wand>{};
        server.run(large_binary_body, server.address);
        server.stop();
    };
    "reject ambiguous framing and invalid headers before handler"_test = [] {
        auto server = server_fixture<Wand>{};
        for (
            auto fields :
            {"Content-Length: 0\r\nContent-Length: 0\r\n",
             "Content-Length: 0, 0\r\n",
             "Content-Length: 0\r\nContent-Length: 2\r\n",
             "Content-Length: +0\r\n",
             "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
             "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n",
             "Transfer-Encoding: gzip, chunked\r\n",
             "Transfer-Encoding: identity\r\n",
             "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
             "Content-Length : 0\r\n",
             "X: a\r\n folded\r\n",
             "Bad Name: a\r\n",
             "Host: other\r\n",
             "Upgrade: websocket\r\nConnection: upgrade\r\n"})
            server.run(
                exchange,
                server.address,
                std::string{"POST / HTTP/1.1\r\nHost: h\r\n"} + fields
                    + "\r\n",
                400,
                "");
        server.run(
            exchange, server.address, "GET / HTTP/1.1\r\n\r\n", 400, "");
        server.run(
            exchange,
            server.address,
            "CONNECT h:443 HTTP/1.1\r\nHost: h\r\n\r\n",
            501,
            "");
        server.run(
            exchange,
            server.address,
            "GET / HTTP/1.0\r\nHost: h\r\n\r\n",
            505,
            "");
        server.run(
            exchange,
            server.address,
            "GET http://evil/ HTTP/1.1\r\nHost: h\r\n\r\n",
            400,
            "");
        server.run(
            exchange,
            server.address,
            std::string{"GET / HTTP/1.1\r\nHost: h\r\nX: a"} + '\0'
                + "b\r\n\r\n",
            400,
            "");
        server.run(
            exchange,
            server.address,
            std::string{
                "POST / HTTP/1.1\r\nHost: h\r\n"
                "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"}
                + get("/smuggled"),
            400,
            "");
        expect(server.state.requests.empty());
        server.stop();
    };
    "expectations rejected without waiting for body"_test = [] {
        auto server = server_fixture<Wand>{};
        server.run(
            exchange,
            server.address,
            "POST / HTTP/1.1\r\nHost: h\r\n"
            "Content-Length: 2\r\nExpect: 100-continue\r\n\r\n",
            417,
            "");
        expect(server.state.requests.empty());
        server.stop();
    };
    "response injection and handler failures become closed 500"_test = [] {
        auto server = server_fixture<Wand>{};
        for (auto path :
             {"/bad-value",
              "/bad-name",
              "/nul",
              "/throw",
              "/cancel",
              "/101"})
            server.run(exchange, server.address, get(path), 500, "");
        server.run(exchange, server.address, get("/after"), 200, "/after:");
        server.stop();
    };
    "malformed chunks and oversized trailers never reach handler"_test =
        [] {
            auto options = http::server_options{};
            options.max_header_bytes = 128;
            auto server = server_fixture<Wand>{options};
            for (auto chunks :
                 {"z\r\na\r\n0\r\n\r\n",
                  "1\r\naX\r\n0\r\n\r\n",
                  "1\r\na\r\n0\r\nContent-Length: 1\r\n\r\n"})
                server.run(
                    exchange,
                    server.address,
                    std::string{"POST / HTTP/1.1\r\nHost: h\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n"}
                        + chunks,
                    400,
                    "");
            server.run(
                exchange,
                server.address,
                std::string{"POST / HTTP/1.1\r\nHost: h\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n0\r\nX: "}
                    + std::string(128, 'x') + "\r\n\r\n",
                431,
                "");
            expect(server.state.requests.empty());
            server.stop();
        };
    "header and payload limits at their boundaries"_test = [] {
        auto options = http::server_options{};
        options.max_connections = 1;
        options.max_header_bytes = 96;
        options.max_body_bytes = 5;
        auto server = server_fixture<Wand>{options};
        auto exact = std::string{
            "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\nX: "};
        exact.append(96 - exact.size() - 4, 'x');
        exact += "\r\n\r\n";
        server.run(exchange, server.address, exact, 200, "/:");
        exact.insert(exact.size() - 4, "x");
        server.run(exchange, server.address, exact, 431, "");
        server.run(
            exchange,
            server.address,
            "POST / HTTP/1.1\r\nHost: h\r\n"
            "Connection: close\r\nContent-Length: 5\r\n\r\nabcde",
            200,
            "/:abcde");
        server.run(
            exchange,
            server.address,
            "POST / HTTP/1.1\r\nHost: h\r\n"
            "Content-Length: 6\r\n\r\n",
            413,
            "");
        server.run(
            exchange,
            server.address,
            "POST / HTTP/1.1\r\nHost: h\r\n"
            "Transfer-Encoding: chunked\r\n\r\n6\r\nabcdef\r\n0\r\n\r\n",
            413,
            "");
        server.stop();
    };
    "wire overhead and response limits"_test = [] {
        auto options = http::server_options{};
        options.max_connections = 1;
        options.max_body_wire_bytes = 12;
        options.max_response_body_bytes = 10;
        auto server = server_fixture<Wand>{options};
        server.run(
            exchange,
            server.address,
            "POST / HTTP/1.1\r\nHost: h\r\n"
            "Connection: close\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n",
            200,
            "/:x");
        server.run(
            exchange,
            server.address,
            "POST / HTTP/1.1\r\nHost: h\r\n"
            "Transfer-Encoding: chunked\r\n\r\n1;long=extension\r\nx\r\n0\r\n\r\n",
            413,
            "");
        server.run(exchange, server.address, get("/large"), 500, "");
        server.stop();
    };
    "response header limit applies before serialization"_test = [] {
        auto options = http::server_options{};
        options.max_response_header_bytes = 128;
        auto server = server_fixture<Wand>{options};
        server.run(exchange, server.address, get("/managed"), 500, "");
        server.run(exchange, server.address, get("/after"), 200, "/after:");
        server.stop();
    };
    "request limit closes on final allowed response"_test = [] {
        auto options = http::server_options{};
        options.max_requests_per_connection = 2;
        auto server = server_fixture<Wand>{options};
        server.run(limited_pipeline, server.address);
        server.stop();
        expect(server.state.requests.size() == 2);
    };
    "handlers overlap across connections"_test = [] {
        auto options = http::server_options{};
        options.max_connections = 2;
        auto server = server_fixture<Wand>{options};
        server.run(overlap, server.address, server.state);
        server.stop();
    };
    "connection cap queues acceptance and reuses worker"_test = [] {
        auto options = http::server_options{};
        options.max_connections = 1;
        auto server = server_fixture<Wand>{options};
        server.run(connection_limit, server.address, server.state);
        server.stop();
    };
    "header body and handler timeouts drain and recover"_test = [] {
        auto options = http::server_options{};
        options.max_connections = 1;
        options.header_timeout = 5ms;
        options.body_timeout = 5ms;
        options.handler_timeout = 5ms;
        auto server = server_fixture<Wand>{options};
        server.run(idle_timeout, server.address);
        server.run(
            exchange, server.address, "GET / HTTP/1.1\r\nHost: ", 408, "");
        server.run(
            exchange,
            server.address,
            "POST / HTTP/1.1\r\nHost: h\r\n"
            "Content-Length: 2\r\n\r\nx",
            408,
            "");
        server.run(exchange, server.address, get("/slow"), 504, "");
        server.run(exchange, server.address, get("/after"), 200, "/after:");
        server.stop();
    };
    "write timeout frees sole worker"_test = [] {
        auto options = http::server_options{};
        options.max_connections = 1;
        options.write_timeout = 5ms;
        auto server = server_fixture<Wand>{options};
        server.run(stalled_writer, server.address, server.state);
        server.stop();
    };
    "disconnects and truncated bodies do not stop server"_test = [] {
        auto options = http::server_options{};
        options.max_connections = 1;
        auto server = server_fixture<Wand>{options};
        server.run(truncated_body, server.address);
        server.run(disconnect_then_recover, server.address);
        server.stop();
    };
    "cancel drains accepted sockets and leaves listener borrowed"_test =
        [] {
            auto options = http::server_options{};
            options.max_connections = 4;
            auto server = server_fixture<Wand>{options};
            auto peers = std::vector<nxt::unique_fd>{};
            server.run(
                park_connections, server.address, peers, server.state);
            expect(server.state.active == 1);
            server.stop();
            server.run(peers_closed, peers);
            // A new server can use the very same listener after the first
            // drains.
            auto restart = nxtrt::root_task{
                server.deck, [&server] {
                    return http::serve(
                        server.listener.get(),
                        [&server](http::request req) {
                            return handle(server.state, std::move(req));
                        });
                }};
            restart.start();
            server.run(
                exchange,
                server.address,
                get("/restart"),
                200,
                "/restart:");
            restart.inner().request_stop();
            server.wand.run_until_done(server.deck, restart.inner());
            std::move(restart.inner()).result();
        };
    "cancel drains an in-progress write before closing its descriptor"_test =
        [] {
            auto options = http::server_options{};
            options.max_connections = 1;
            auto server = server_fixture<Wand>{options};
            auto peer = nxt::unique_fd{};
            server.run(park_write, server.address, peer);
            expect(server.state.requests.size() == 1);
            server.stop();
            server.run(partial_response_closed, peer.get());
        };
    "cancel before each server startup turn is safe"_test = [] {
        for (int turns = 0; turns < 16; ++turns) {
            auto options = http::server_options{};
            options.max_connections = 2;
            auto server = server_fixture<Wand>{options};
            for (int i = 0; i < turns; ++i)
                server.deck.run_ready();
            server.stop();
            expect(server.state.requests.empty());
        }
    };
#if defined(__linux__)
    "shutdown releases worker bells and accepted descriptors"_test = [] {
        auto count = [] {
            return std::distance(
                std::filesystem::directory_iterator{"/proc/self/fd"},
                std::filesystem::directory_iterator{});
        };
        auto before = count();
        {
            auto options = http::server_options{};
            options.max_connections = 4;
            auto server = server_fixture<Wand>{options};
            auto peers = std::vector<nxt::unique_fd>{};
            server.run(
                park_connections, server.address, peers, server.state);
            server.stop();
            server.run(peers_closed, peers);
        }
        expect(count() == before);
    };
#endif
    "many requests reuse per-operation scopes"_test.slow().with_timeout(30s) = [] {
        auto options = http::server_options{};
        options.max_connections = 1;
        options.max_requests_per_connection = 5000;
        auto server = server_fixture<Wand>{options};
        server.run(repeated_requests, server.address, 4100, false);
        server.stop();
        expect(server.state.requests.size() == 4100);
    };
    "many connections reuse fixed workers"_test.slow().with_timeout(30s) = [] {
        auto options = http::server_options{};
        options.max_connections = 1;
        auto server = server_fixture<Wand>{options};
        server.run(repeated_requests, server.address, 4100, true);
        server.stop();
        expect(server.state.requests.size() == 4100);
    };
}

#if defined(__linux__)
static suite http_server_epoll_tests{
    "HTTP server epoll", server_tests<nxtrt::epoll_wand>};
static suite http_server_uring_tests{
    "HTTP server uring", server_tests<nxtrt::uring_wand>};
#else
static suite http_server_kqueue_tests{
    "HTTP server kqueue", server_tests<nxtrt::kqueue_wand>};
#endif

} // namespace
} // namespace nxt::test
