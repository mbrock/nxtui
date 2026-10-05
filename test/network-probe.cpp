// Portable consumer/probe. Never loads credentials until --openai is
// selected.
#include <nxtrt/arch.hpp>
#include <nxtai/agent.hpp>
#include <nxtai/responses_transport.hpp>

#include <cstdio>
#include <fstream>
#include <tuple>

using namespace std::chrono_literals;
using namespace nxtrt;

namespace {
void check(bool ok, const char * message)
{
    if (!ok)
        throw runtime_error{message};
}

struct observer
{
    std::string output;
    std::size_t deltas = 0;
    bool print = false;

    task<void> text(std::string delta)
    {
        output += delta;
        ++deltas;
        if (print) {
            std::fwrite(delta.data(), 1, delta.size(), stdout);
            std::fflush(stdout);
        }
        co_return;
    }
};

template<typename T>
T probe_run(task<T> work)
{
    auto wand = arch::wand{};
    auto d = deck{&wand};
    auto root =
        root_task{d, [&] { return with_timeout(60s, std::move(work)); }};
    root.start();
    wand.run_until_done(d, root.inner());
    return std::move(root.inner()).result();
}

task<void> dns()
{
    auto addresses = co_await net::resolve_tcp("localhost", "443");
    auto loopback = false;
    for (auto const & a : addresses)
        if (a.family == AF_INET) {
            auto * in =
                reinterpret_cast<sockaddr_in const *>(a.sockaddr_ptr());
            loopback |= ntohl(in->sin_addr.s_addr) == INADDR_LOOPBACK
                        && ntohs(in->sin_port) == 443;
        }
    check(loopback, "DNS did not return IPv4 localhost:443");
    auto rejected = false;
    try {
        (void) co_await net::resolve_tcp(
            "nxt-probe-does-not-exist.invalid", "443");
    } catch (std::exception const &) {
        rejected = true;
    }
    check(rejected, "invalid DNS name succeeded");
}

task<void> echo_server(socket_handle listener)
{
    std::array<std::byte, 32> tx{}, rx{};
    auto peer = net::socket{co_await net::accept(listener), tx, rx};
    auto input = std::string{};
    while (input.size() < 7) {
        auto b = co_await peer.input().take(7 - input.size());
        check(!b.empty(), "early server EOF");
        input.append(reinterpret_cast<const char *>(b.data()), b.size());
    }
    check(
        input == std::string{"a\0bcXYZ", 7},
        "buffered server payload mismatch");
    co_await write(peer.output(), input.substr(0, 2));
    co_await peer.output().flush();
    co_await write(peer.output(), input.substr(2));
    co_await peer.output().flush();
}

task<void> echo_client(std::string port)
{
    std::array<std::byte, 32> tx{}, rx{};
    auto socket =
        net::socket{co_await net::connect_tcp("localhost", port), tx, rx};
    co_await write(socket.output(), std::string_view{"a\0bcXYZ", 7});
    co_await socket.output().flush();
    auto reply = std::string{};
    while (auto b = co_await socket.input().take_some())
        reply.append(reinterpret_cast<const char *>(b->data()), b->size());
    check(
        reply == std::string{"a\0bcXYZ", 7},
        "buffered client payload or EOF mismatch");
}

task<void> sockets()
{
    auto listener = net::listen_tcp_loopback();
    net::attach_socket(listener.get());
    auto port =
        std::to_string(ntohs(net::socket_address(listener.get()).sin_port));
    co_await when_all(echo_server(listener.get()), echo_client(port));
}

nxtai::responses::openai_responses_request fixture_request()
{
    return {.api_key = "fixture-not-a-secret", .input = "fixture"};
}

task<void> invalid_keys()
{
    auto transport = nxtai::responses_transport{{.host = "unused.invalid"}};
    auto view = observer{};
    for (auto key :
         {std::string{},
          std::string{"abc\r\nX: injected"},
          std::string{"abc\0xyz", 7}}) {
        auto request = fixture_request();
        request.api_key = std::move(key);
        auto rejected = false;
        try {
            (void) co_await transport(std::move(request), view);
        } catch (invalid_argument const &) {
            rejected = true;
        }
        check(rejected, "unsafe credential was not rejected before I/O");
    }
}

task<void> streamed(nxtai::responses_transport_options options)
{
    auto transport = nxtai::responses_transport{std::move(options)};
    auto view = observer{};
    auto result = co_await transport(fixture_request(), view);
    check(
        view.output == std::string{"alpha\0omega", 11},
        "streamed text mismatch");
    check(view.deltas == 2, "streamed delta count mismatch");
    check(
        result.id == "fixture-response" && result.output_items.size() == 1,
        "completed snapshot mismatch");
}

task<void>
rejected(nxtai::responses_transport_options options, std::string expected)
{
    auto transport = nxtai::responses_transport{std::move(options)};
    auto view = observer{};
    auto did_reject = false;
    try {
        (void) co_await transport(fixture_request(), view);
    } catch (runtime_error const & e) {
        did_reject = std::string_view{e.what()}.find(expected)
                     != std::string_view::npos;
    }
    check(did_reject, "request did not reject with expected error");
}

task<void> stop_after_delta(observer & view)
{
    while (!view.deltas)
        co_await op::timeout::after(2ms);
}

task<void> cancel_stream(nxtai::responses_transport_options options)
{
    auto transport = nxtai::responses_transport{options};
    auto view = observer{};
    auto [response, stopper] = co_await settle(
        std::tuple{
            transport(fixture_request(), view), stop_after_delta(view)},
        first_completion_group{});
    check(bool(stopper), "stream never reached first delta");
    check(
        !response && is_operation_cancelled(response.error()),
        "stream did not cancel and drain");
    check(view.output == "partial", "unexpected cancelled stream text");
    // Reuse the same deck/IOCP port after every drain, not just a fresh
    // deck.
    options.target = "/stream";
    co_await streamed(std::move(options));
}

#if defined(_WIN32)
void cancel_dns()
{
    // Fresh names avoid a cached synchronous response. Stop while the
    // native query is outstanding, then resolve successfully on the same
    // deck.
    auto wand = arch::wand{};
    auto d = deck{&wand};
    for (int i = 0; i < 3; ++i) {
        auto name = "nxt-cancel-" + std::to_string(GetTickCount64()) + "-"
                    + std::to_string(i) + ".invalid";
        auto root =
            root_task{d, [&] { return net::resolve_tcp(name, "443"); }};
        root.start();
        d.run_ready();
        check(
            !root.inner().done(), "DNS cancellation needs a pending query");
        root.inner().request_stop();
        wand.run_until_done(d, root.inner());
        auto cancelled = false;
        try {
            (void) std::move(root.inner()).result();
        } catch (operation_cancelled const &) {
            cancelled = true;
        }
        check(cancelled, "native DNS did not cancel and drain");
    }
    auto root = root_task{d, dns};
    root.start();
    wand.run_until_done(d, root.inner());
    std::move(root.inner()).result();
}
#endif

std::string read_key(const char * path)
{
#if !defined(_WIN32)
    if (std::string_view{path} == "--env") {
        auto * key = std::getenv("OPENAI_API_KEY");
        check(key != nullptr, "OPENAI_API_KEY is not set");
        return key;
    }
#endif
    auto file = std::ifstream{path, std::ios::binary};
    check(bool(file), "cannot open runtime credential file");
    auto key = std::string{std::istreambuf_iterator<char>{file}, {}};
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r'))
        key.pop_back();
    check(
        !key.empty() && key.size() < 4096,
        "invalid runtime credential file size");
    return key;
}

task<void> openai(std::string key, std::string ca_file)
{
    auto transport =
        nxtai::responses_transport{{.ca_file = std::move(ca_file)}};
    auto request = nxtai::responses::openai_responses_request{
        .api_key = std::move(key),
        .model = "gpt-6-luna",
        .input = "Reply with just: Xbox transport online",
        .include = {"reasoning.encrypted_content"},
        .max_output_tokens = 1024};
    auto view = observer{};
    view.print = true;
    auto response = co_await transport(request, view);
    check(
        !view.output.empty() && view.deltas != 0
            && !response.output_items.empty(),
        "empty model response");
    std::printf(
        "\nMODEL gpt-6-luna: %zu streamed deltas, completed %s\n",
        view.deltas,
        response.id.c_str());
    // A second turn retains the opaque output/reasoning items, rather than
    // replacing the model or using a second transport.
    request.input_items =
        nxtai::responses::input_items_from_request(request);
    for (auto & item : response.output_items)
        request.input_items.push_back(std::move(item));
    request.input_items.push_back(
        {R"({"role":"user","content":"What hardware did my first requested phrase name? Reply with one word."})"});
    view.output.clear();
    view.deltas = 0;
    response = co_await transport(request, view);
    check(
        !view.output.empty() && view.deltas != 0
            && !response.output_items.empty(),
        "empty second model response");
    std::printf(
        "\nMODEL gpt-6-luna turn 2: %zu streamed deltas, completed %s\n",
        view.deltas,
        response.id.c_str());
}
} // namespace

int main(int argc, char ** argv)
{
    auto failures = 0;
    auto tests = 0;
    auto test = [&](const char * name, auto fn) {
        ++tests;
        try {
            fn();
            std::printf("PASS %s\n", name);
        } catch (std::exception const & e) {
            ++failures;
            std::printf("FAIL %s: %s\n", name, e.what());
        }
        std::fflush(stdout);
    };
    if (argc == 1) {
        test("unsafe credentials rejected before I/O", [] {
            probe_run(invalid_keys());
        });
        test("DNS localhost port and invalid name", [] {
            probe_run(dns());
        });
        test("socket wrappers buffered echo and EOF", [] {
            probe_run(sockets());
        });
#if defined(_WIN32)
        test(
            "pending native DNS cancellation, drain and reuse", cancel_dns);
#endif
    } else if (argc == 6 && std::string_view{argv[1]} == "--fixture") {
        auto options = nxtai::responses_transport_options{
            .host = argv[2],
            .service = argv[3],
            .target = "/stream",
            .ca_file = argv[4]};
        test("TLS verified chunked gzip SSE", [&] {
            probe_run(streamed(options));
        });
        test("untrusted chain rejected", [&] {
            auto o = options;
            o.ca_file = argv[5];
            probe_run(rejected(o, "certificate verification failed"));
        });
        test("wrong SAN hostname rejected", [&] {
            auto o = options;
            o.host = "127.0.0.1";
            probe_run(rejected(o, "certificate verification failed"));
        });
        test("missing trust file rejected", [&] {
            auto o = options;
            o.ca_file += ".missing";
            probe_run(rejected(o, "CA"));
        });
        test("HTTP error rejected", [&] {
            auto o = options;
            o.target = "/status";
            probe_run(rejected(o, "HTTP error"));
        });
        test("wrong content-type rejected", [&] {
            auto o = options;
            o.target = "/type";
            probe_run(rejected(o, "content-type"));
        });
        test("truncated Responses stream rejected", [&] {
            auto o = options;
            o.target = "/truncated";
            probe_run(rejected(o, "before completion"));
        });
        test("stream cancellation and completion drain", [&] {
            auto o = options;
            o.target = "/stall";
            probe_run(cancel_stream(o));
        });
        test("fresh connection after cancelled stream", [&] {
            probe_run(streamed(options));
        });
    } else if (argc == 4 && std::string_view{argv[1]} == "--openai") {
        test("real two-turn GPT-6 Luna conversation", [&] {
            probe_run(openai(read_key(argv[2]), argv[3]));
        });
    } else {
        std::fprintf(
            stderr,
            "usage: network-probe [--fixture HOST PORT CA WRONG_CA | --openai KEY_FILE CA]\n");
        return 2;
    }
    std::printf("NETWORK: %d tests, %d failures\n", tests, failures);
    return failures ? 1 : 0;
}
