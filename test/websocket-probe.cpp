// Portable installed consumer; no credentials or external services.
#include <nxtrt/arch.hpp>
#include <nxtrt/websocket.hpp>

#include <cstdio>
#include <tuple>

using namespace nxtrt;
using namespace std::chrono_literals;
namespace ws = nxtrt::websocket;

namespace {
void check(bool ok, const char * why)
{
    if (!ok)
        throw runtime_error{why};
}

void run(task<> work)
{
    auto wand = arch::wand{};
    auto d = deck{&wand};
    auto root =
        root_task{d, [&] { return with_timeout(15s, std::move(work)); }};
    root.start();
    wand.run_until_done(d, root.inner());
    std::move(root.inner()).result();
}

task<> exchange(std::string base, ws::options options)
{
    auto c = co_await ws::connect(base + "/exchange", options);
    // Length boundaries exercise all three client length encodings; compare
    // asymmetric binary bytes, not merely sizes or absence of exceptions.
    for (auto size : {0, 125, 126, 65535, 65536}) {
        auto payload = std::string(size, '\0');
        for (int i = 0; i < size; ++i)
            payload[i] = static_cast<char>((i * 37 + 11) & 255);
        co_await c->send(ws::message_type::binary, payload);
        auto reply = co_await c->receive();
        check(
            reply && reply->type == ws::message_type::binary
                && reply->data == payload,
            "binary echo mismatch");
    }
    co_await c->send(ws::message_type::text, "hello \xf0\x9f\x8c\x99");
    auto text = co_await c->receive();
    check(
        text && text->type == ws::message_type::text
            && text->data == "hello \xf0\x9f\x8c\x99",
        "text echo mismatch");
    co_await c->ping(std::string{"p\0q", 3});
    auto pong = co_await c->receive();
    check(
        pong && pong->type == ws::message_type::pong
            && pong->data == std::string{"p\0q", 3},
        "pong mismatch");
    co_await c->close(4001, "done");
    auto closed = co_await c->receive();
    check(
        closed && closed->type == ws::message_type::close
            && closed->code == 4001 && closed->data == "done",
        "close mismatch");
    check(!(co_await c->receive()), "duplicate close event");
    // Owned earlier event remains unchanged after subsequent reads/close.
    check(
        text->data == "hello \xf0\x9f\x8c\x99", "borrowed message storage");
}

task<> fragments(std::string base, ws::options options)
{
    options.max_message_size = 6;
    auto c = co_await ws::connect(base + "/fragments", options);
    auto pong = co_await c->receive();
    check(
        pong && pong->type == ws::message_type::pong
            && pong->data == "unsolicited",
        "interleaved pong missing");
    auto text = co_await c->receive();
    check(
        text && text->type == ws::message_type::text
            && text->data == "A\xf0\x9f\x8c\x99Z",
        "fragmented UTF-8 mismatch");
    auto binary = co_await c->receive();
    check(
        binary && binary->type == ws::message_type::binary
            && binary->data == std::string{"\0\xff\x13\0", 4},
        "fragmented binary mismatch");
    auto closed = co_await c->receive();
    check(
        closed && closed->type == ws::message_type::close && !closed->code
            && closed->data.empty(),
        "empty server close mismatch");
}

task<> close_mid(std::string base, ws::options options)
{
    auto c = co_await ws::connect(base + "/close-mid", options);
    auto closed = co_await c->receive();
    check(
        closed && closed->type == ws::message_type::close
            && closed->code == 1000 && closed->data == "bye",
        "close interrupted message not discarded");
}

task<> reject(std::string url, ws::options options, bool handshake)
{
    auto rejected = false;
    auto path = std::string_view{url}.substr(url.rfind('/') + 1);
    auto reason = std::string_view{};
    if (path == "bad-length16" || path == "bad-length64"
        || path == "bad-high-bit")
        reason = "noncanonical WebSocket length";
    else if (path.starts_with("bad-control-"))
        reason = "invalid WebSocket control frame";
    else if (path == "bad-limit" || path == "bad-fragment-limit")
        reason = "WebSocket message limit exceeded";
    else if (
        path == "bad-rsv" || path == "bad-mask" || path == "bad-opcode")
        reason = "invalid WebSocket frame flags/opcode";
    else if (path == "bad-continuation" || path == "bad-new-message")
        reason = "invalid WebSocket fragmentation";
    else if (path.starts_with("bad-utf8"))
        reason = "invalid WebSocket UTF-8";
    else if (path.starts_with("bad-close-"))
        reason = "invalid WebSocket close";
    auto c = std::unique_ptr<ws::client>{};
    try {
        c = co_await ws::connect(url, options);
        check(!handshake, "invalid handshake accepted");
        (void) co_await c->receive();
    } catch (ws::protocol_error const & e) {
        // A later EOF or a size-limit error must not mask a missing flags,
        // fragmentation or canonical-length check.
        rejected = std::string_view{e.what()}.find(reason)
                   != std::string_view::npos;
    } catch (end_of_stream const &) {
        // TLS rejects transport truncation before the WebSocket parser can
        // observe EOF. Other malformed frames must fail at the framing
        // layer.
        rejected =
            url.ends_with("/bad-eof") || url.ends_with("/bad-truncated");
    }
    check(rejected, "malformed WebSocket accepted");
    if (c) {
        auto terminal = false;
        try {
            (void) co_await c->receive();
        } catch (logic_error const &) {
            terminal = true;
        }
        check(terminal, "protocol failure left connection reusable");
    }
}

task<> trust_rejected(std::string url, ws::options options)
{
    auto rejected = false;
    try {
        (void) co_await ws::connect(url, options);
    } catch (runtime_error const & e) {
        rejected = std::string_view{e.what()}.find(
                       "certificate verification failed")
                   != std::string_view::npos;
    }
    check(rejected, "TLS trust/hostname not enforced");
}

task<> overlapping(ws::client & client)
{
    co_await op::timeout::after(20ms);
    auto rejected = false;
    try {
        co_await client.ping();
    } catch (logic_error const &) {
        rejected = true;
    }
    check(rejected, "overlapping operation not rejected");
}

task<> stop_later(std::chrono::milliseconds delay)
{
    co_await op::timeout::after(delay);
}

std::string describe(outcome<void> const & result)
{
    if (result)
        return "completed";
    if (is_operation_cancelled(result.error()))
        return "cancelled";
    try {
        rethrow(result.error());
    } catch (std::system_error const & e) {
        return "failed code=" + std::to_string(e.code().value())
               + " category=" + e.code().category().name() + " " + e.what();
    } catch (std::exception const & e) {
        return "failed " + std::string{e.what()};
    } catch (...) {
        return "failed non-standard exception";
    }
}

task<> cancelled(
    std::string base,
    ws::options options,
    std::string stage,
    std::chrono::milliseconds delay)
{
    if (stage == "upgrade") {
        auto [connection, timer] = co_await settle(
            std::tuple{
                ws::connect(base + "/stall-upgrade", options),
                stop_later(delay)},
            first_completion_group{});
        check(
            timer && !connection
                && is_operation_cancelled(connection.error()),
            "Upgrade cancellation failed");
    } else {
        options.max_message_size = 16 * 1024 * 1024;
        auto c = co_await ws::connect(base + "/stall-" + stage, options);
        auto ready = co_await c->receive();
        check(
            ready && ready->type == ws::message_type::pong
                && ready->data == "ready",
            "cancellation barrier missing");
        if (stage == "send") {
            auto started = std::chrono::steady_clock::now();
            auto [sent, timer] = co_await settle(
                std::tuple{
                    c->send(
                        ws::message_type::binary,
                        std::string(options.max_message_size, 'x')),
                    stop_later(delay)},
                first_completion_group{});
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            auto sent_text = describe(sent);
            auto timer_text = describe(timer);
            std::printf(
                "SEND-CANCEL %s payload=%zu delay_ms=%lld elapsed_ms=%lld send=%s timer=%s\n",
                base.c_str(),
                options.max_message_size,
                static_cast<long long>(delay.count()),
                static_cast<long long>(elapsed),
                sent_text.c_str(),
                timer_text.c_str());
            std::fflush(stdout);
            auto diagnostic = "send did not cancel (send=" + sent_text
                              + "; timer=" + timer_text + ")";
            check(
                timer && !sent && is_operation_cancelled(sent.error()),
                diagnostic.c_str());
        } else if (stage == "overlap") {
            auto [read, attempt] = co_await settle(
                std::tuple{c->receive(), overlapping(*c)},
                first_completion_group{});
            check(
                attempt && !read && is_operation_cancelled(read.error()),
                "overlap damaged pending reader");
        } else {
            auto [read, timer] = co_await settle(
                std::tuple{c->receive(), stop_later(delay)},
                first_completion_group{});
            check(
                timer && !read && is_operation_cancelled(read.error()),
                "read did not cancel");
        }
        auto terminal = false;
        try {
            co_await c->ping();
        } catch (logic_error const &) {
            terminal = true;
        }
        check(terminal, "cancelled connection reusable");
    }
    // Same deck and IOCP port: no late completion may corrupt the new
    // socket.
    co_await exchange(base, options);
}

task<> invalid_inputs(std::string base, ws::options options)
{
    for (auto url :
         {"http://unused",
          "ws://user@unused/",
          "ws://unused/#x",
          "ws://unused/\r\nInjected: x",
          "ws://unused:0/",
          "ws://unused:65536/",
          "ws://[::1]/",
          "ws://unused?q"}) {
        auto rejected = false;
        try {
            (void) co_await ws::connect(url);
        } catch (invalid_argument const &) {
            rejected = true;
        }
        check(rejected, "unsafe URL accepted");
    }
    options.max_message_size = 6;
    auto c = co_await ws::connect(base + "/exchange", options);
    for (auto bad :
         {std::string{"\xc0\xaf"},
          std::string{"\xed\xa0\x80"},
          std::string{"\xf4\x90\x80\x80"},
          std::string{"\xe2\x82"}}) {
        auto rejected = false;
        try {
            co_await c->send(ws::message_type::text, bad);
        } catch (invalid_argument const &) {
            rejected = true;
        }
        check(rejected, "invalid outgoing UTF-8 accepted");
    }
    auto rejected = false;
    try {
        co_await c->close(1005);
    } catch (invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "invalid outgoing close accepted");
    rejected = false;
    try {
        co_await c->ping(std::string(126, 'x'));
    } catch (invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "oversized outgoing ping accepted");
    rejected = false;
    try {
        co_await c->send(ws::message_type::binary, "1234567");
    } catch (invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "oversized outgoing message accepted");
    co_await c->send(ws::message_type::binary, "abcdef");
    auto exact = co_await c->receive();
    check(exact && exact->data == "abcdef", "exact message limit rejected");
    co_await c->close();
    check(
        (co_await c->receive())->code == 1000,
        "invalid input poisoned connection");
}
} // namespace

int main(int argc, char ** argv)
{
    if (argc != 5 && argc != 6) {
        std::fprintf(
            stderr,
            "usage: websocket-probe WS_BASE WSS_BASE CA WRONG_CA [CANCEL_MS=100]\n");
        return 2;
    }
    auto cancel_ms = 100;
    if (argc == 6) {
        auto text = std::string_view{argv[5]};
        auto [end, error] = std::from_chars(
            text.data(), text.data() + text.size(), cancel_ms);
        if (error != std::errc{} || end != text.data() + text.size()
            || cancel_ms < 20 || cancel_ms > 5000) {
            std::fprintf(stderr, "CANCEL_MS must be 20..5000\n");
            return 2;
        }
    }
    int tests = 0, failures = 0;
    auto test = [&](std::string name, task<> work) {
        ++tests;
        try {
            run(std::move(work));
            std::printf("PASS %s\n", name.c_str());
        } catch (std::exception const & e) {
            ++failures;
            std::printf("FAIL %s: %s\n", name.c_str(), e.what());
        }
        std::fflush(stdout);
    };
    check(
        ws::detail::accept_key("dGhlIHNhbXBsZSBub25jZQ==")
            == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
        "RFC6455 accept vector");
    for (auto base : {std::string{argv[1]}, std::string{argv[2]}}) {
        auto options = ws::options{.ca_file = argv[3]};
        test(
            base + " masked lengths/echo/ping/close",
            exchange(base, options));
        test(
            base + " fragments/interleaved controls",
            fragments(base, options));
        test(
            base + " close interrupts fragments", close_mid(base, options));
        test(base + " invalid local input", invalid_inputs(base, options));
        for (auto path :
             {"accept",
              "missing-accept",
              "status",
              "status-digits",
              "status-control",
              "version",
              "connection",
              "upgrade",
              "duplicate",
              "extension",
              "subprotocol",
              "malformed",
              "folded"})
            test(
                base + " reject handshake " + path,
                reject(base + "/bad-" + path, options, true));
        for (auto path :
             {"rsv",          "mask",         "opcode",
              "continuation", "new-message",  "control-fin",
              "control-size", "length16",     "length64",
              "high-bit",     "utf8",         "utf8-surrogate",
              "utf8-large",   "utf8-partial", "close-one",
              "close-code",   "close-reason", "eof",
              "truncated",    "limit",        "fragment-limit"}) {
            auto o = options;
            o.max_message_size = 6;
            test(
                base + " reject frame " + path,
                reject(base + "/bad-" + path, o, false));
        }
        for (auto stage :
             {"upgrade",
              "header",
              "payload",
              "fragment",
              "send",
              "overlap"})
            test(
                base + " cancel/drain/reconnect " + stage,
                cancelled(
                    base,
                    options,
                    stage,
                    std::chrono::milliseconds{cancel_ms}));
    }
    test(
        "wss untrusted CA",
        trust_rejected(
            std::string{argv[2]} + "/exchange", {.ca_file = argv[4]}));
    if (ws::detail::parse_url(argv[2]).host == "localhost") {
        auto ip = std::string{argv[2]};
        ip.replace(ip.find("localhost"), 9, "127.0.0.1");
        test(
            "wss wrong SAN",
            trust_rejected(ip + "/exchange", {.ca_file = argv[3]}));
    } else {
        std::printf(
            "SKIP wrong SAN: requires localhost/IP pair; remote IP SAN verified by successful wss\n");
    }
    std::printf("WEBSOCKET: %d tests, %d failures\n", tests, failures);
    return failures ? 1 : 0;
}
