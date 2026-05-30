// A deliberately small OpenAI Responses streaming client.
//
// This is a plain byte_reader/byte_writer program: TCP socket -> TLS reader ->
// HTTP response body reader -> SSE events -> stdout fd writer. No UI runtime,
// no HUD, no tool loop.

#include "nxtrt/task.hpp"

#include <nxtrt/buffers.hpp>
#include <nxtrt/http.hpp>
#include <nxtrt/net_dns.hpp>
#include <nxtrt/tls.hpp>
#include <nxtrt/wire.hpp>
#include <nxt/json.hpp>

#if defined(__linux__)
#  include <nxtrt/uring_wand.hpp>
#else
#  include <nxtrt/kqueue_wand.hpp>
#endif

#include <charconv>
#include <cstdlib>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct cli_options
{
    std::string model = "gpt-5-mini";
    std::string prompt = "Say hello in one sentence.";
    std::size_t max_output_tokens = 1024;
};

std::string_view arg_view(char * arg)
{
    return arg == nullptr ? std::string_view{} : std::string_view{arg};
}

std::size_t parse_size(std::string_view text, std::string_view name)
{
    auto value = std::size_t{};
    auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size())
        throw nxtrt::runtime_error{"invalid " + std::string{name}};
    return value;
}

cli_options parse_cli(int argc, char ** argv)
{
    auto options = cli_options{};
    auto prompt_parts = std::vector<std::string_view>{};

    for (auto i = 1; i < argc; ++i) {
        auto arg = arg_view(argv[i]);
        if (arg == "--model") {
            if (++i >= argc)
                throw nxtrt::runtime_error{"--model needs a value"};
            options.model = arg_view(argv[i]);
        } else if (arg == "--max-output") {
            if (++i >= argc)
                throw nxtrt::runtime_error{"--max-output needs a value"};
            options.max_output_tokens =
                parse_size(arg_view(argv[i]), "--max-output");
        } else {
            prompt_parts.push_back(arg);
        }
    }

    if (!prompt_parts.empty()) {
        options.prompt.clear();
        for (auto part : prompt_parts) {
            if (!options.prompt.empty())
                options.prompt.push_back(' ');
            options.prompt += part;
        }
    }

    return options;
}

std::string accept_encoding_header()
{
    auto value = std::string{"gzip, deflate"};
#if defined(NXTRT_HAVE_ZSTD)
    value += ", zstd";
#endif
#if defined(NXTRT_HAVE_BROTLI)
    value += ", br";
#endif
    return value;
}

std::string responses_body(const cli_options & options)
{
    auto json = nxt::json::writer{};
    json.character('{');
    json.key("model");
    json.string(options.model);
    json.character(',');
    json.key("stream");
    json.boolean(true);
    json.character(',');
    json.key("store");
    json.boolean(false);
    json.character(',');
    json.key("max_output_tokens");
    json.number(options.max_output_tokens);
    json.character(',');
    json.key("input");
    json.string(options.prompt);
    json.character('}');
    return std::move(json.out);
}

nxtrt::http::request openai_request(
    const cli_options & options,
    std::string_view api_key)
{
    return nxtrt::http::request{
        .method = "POST",
        .target = "/v1/responses",
        .host = "api.openai.com",
        .headers =
            {
                {"User-Agent", "nxt-openai-sse-demo/0"},
                {"Accept", "text/event-stream"},
                {"Accept-Encoding", accept_encoding_header()},
                {"Content-Type", "application/json"},
                {"Authorization", "Bearer " + std::string{api_key}},
                {"Connection", "close"},
            },
        .body = responses_body(options),
    };
}

bool response_status_is_success(const nxtrt::http::response_head & head)
{
    return head.status >= 200 && head.status < 300;
}

bool response_content_type_is(
    const nxtrt::http::response_head & head,
    std::string_view expected)
{
    auto value = nxtrt::http::header_value(head, "content-type");
    if (!value)
        return false;
    auto semicolon = value->find(';');
    auto media_type = nxtrt::http::trim_ascii(value->substr(0, semicolon));
    return nxtrt::http::iequals(media_type, expected);
}

struct tls_handshake_event
{
    nxtrt::tls::handshake_progress_kind kind =
        nxtrt::tls::handshake_progress_kind::event;
    std::string name;
};

std::string_view tls_handshake_event_kind_name(
    nxtrt::tls::handshake_progress_kind kind)
{
    switch (kind) {
    case nxtrt::tls::handshake_progress_kind::begin:
        return "begin";
    case nxtrt::tls::handshake_progress_kind::end:
        return "end";
    case nxtrt::tls::handshake_progress_kind::event:
        return "event";
    }
    return "event";
}

class tls_handshake_event_tx
{
public:
    explicit tls_handshake_event_tx(
        nxtrt::wire<tls_handshake_event>::tx_side events) noexcept
        : events_(events)
    {}

    nxtrt::task<void> operator()(
        const nxtrt::tls::handshake_progress & progress)
    {
        auto sent = co_await events_.send(
            tls_handshake_event{
                .kind = progress.kind,
                .name = std::string{progress.name},
            });
        if (sent)
            co_await events_.flush();
    }

private:
    nxtrt::wire<tls_handshake_event>::tx_side events_;
};

nxtrt::task<void> print_tls_handshake_events(
    nxtrt::wire<tls_handshake_event>::rx_side events)
{
    auto out = nxtrt::standard_output_writer(4096);
    while (auto event = co_await events.next()) {
        co_await out.print(
            "tls: {} {}\n",
            tls_handshake_event_kind_name(event->kind),
            event->name);
        co_await out.flush();
    }
}

nxtrt::task<void> write_sse_event(
    nxtrt::byte_writer & out,
    const nxtrt::http::server_sent_event & event)
{
    co_await out.print("event: {}\n", event.type);
    if (!event.id.empty())
        co_await out.print("id: {}\n", event.id);
    if (event.retry_ms)
        co_await out.print("retry: {}\n", *event.retry_ms);
    co_await out.write(std::string_view{"data: "});
    co_await out.write(event.data);
    co_await out.write(std::string_view{"\n\n"});
    co_await out.flush();
}

nxtrt::task<void> stream_openai_sse_with_zone(
    cli_options options,
    std::string api_key)
{
    auto request_text =
        nxtrt::http::serialize(openai_request(options, api_key));

    auto socket = co_await nxtrt::net::connect_tcp("api.openai.com", "443");
    auto socket_out =
        nxtrt::socket_sink{socket.get(), 0, std::size_t{16 * 1024}};
    auto socket_in =
        nxtrt::socket_source{socket.get(), 0, std::size_t{64 * 1024}};

    auto tls = nxtrt::tls::tls13_client_session{
        socket_in,
        socket_out,
        std::size_t{64 * 1024},
    };

    auto tls_events = nxtrt::wire<tls_handshake_event>{128};
    auto tls_tx = tls_events.tx();
    nxtrt::fork(print_tls_handshake_events(tls_events.rx()));
    try {
        co_await tls.handshake(
            "api.openai.com",
            tls_handshake_event_tx{tls_tx});
        tls_tx.close();
    } catch (...) {
        tls_tx.close();
        throw;
    }

    co_await tls.write_all(request_text);

    auto head = co_await nxtrt::http::read_response_head(tls);
    if (!response_status_is_success(head))
        throw nxtrt::runtime_error{
            std::format(
                "OpenAI Responses HTTP error: {} {}",
                head.status,
                head.reason)};
    if (!response_content_type_is(head, "text/event-stream"))
        throw nxtrt::runtime_error{
            "OpenAI Responses expected text/event-stream"};

    auto body = nxtrt::http::response_body_decoding_reader(tls, head);
    auto stdout_writer = nxtrt::standard_output_writer(64 * 1024);
    while (auto event = co_await nxtrt::http::parse_sse_event(body)) {
        co_await write_sse_event(stdout_writer, *event);
        if (event->data == "[DONE]")
            break;
    }
}

struct openai_sse_zone_body
{
    cli_options options;
    std::string api_key;

    nxtrt::task<void> operator()()
    {
        return stream_openai_sse_with_zone(
            std::move(options), std::move(api_key));
    }
};

nxtrt::task<void> stream_openai_sse(cli_options options, std::string api_key)
{
    co_await nxtrt::with_zone(
        openai_sse_zone_body{
            .options = std::move(options),
            .api_key = std::move(api_key),
        });
}

void write_stderr(std::string_view text)
{
    while (!text.empty()) {
        auto n = ::write(STDERR_FILENO, text.data(), text.size());
        if (n <= 0)
            return;
        text.remove_prefix(static_cast<std::size_t>(n));
    }
}

} // namespace

int main(int argc, char ** argv)
try {
    auto options = parse_cli(argc, argv);
    auto * api_key = std::getenv("OPENAI_API_KEY");
    if (api_key == nullptr || std::string_view{api_key}.empty())
        throw nxtrt::runtime_error{"OPENAI_API_KEY is not set"};

#if defined(__linux__)
    nxtrt::run(
        [options = std::move(options), api_key = std::string{api_key}]() mutable {
            return stream_openai_sse(std::move(options), std::move(api_key));
        });
#elif NXT_RT_HAS_KQUEUE
    nxtrt::run_with_kqueue(
        [options = std::move(options), api_key = std::string{api_key}]() mutable {
            return stream_openai_sse(std::move(options), std::move(api_key));
        });
#else
    static_assert(NXT_RT_HAS_KQUEUE, "OpenAI SSE demo needs a runtime wand");
#endif

    return 0;
} catch (const std::exception & error) {
    write_stderr(std::format("nxt-openai-sse-demo: {}\n", error.what()));
    return 1;
}
