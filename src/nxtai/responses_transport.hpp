#pragma once

#include <nxtai/responses_request.hpp>
#include <nxtai/responses_stream.hpp>
#include <nxtrt/http.hpp>
#include <nxtrt/net_dns.hpp>
#include <nxtrt/tls.hpp>

#include <array>

namespace nxtai {

struct responses_http_error : nxtrt::runtime_error
{
    explicit responses_http_error(int status, std::string reason)
        : nxtrt::runtime_error{"OpenAI Responses HTTP error"}
        , status(status)
        , reason(std::move(reason))
    {
    }

    int status;
    std::string reason;
};

struct responses_content_type_error : nxtrt::runtime_error
{
    explicit responses_content_type_error(std::optional<std::string> actual)
        : nxtrt::runtime_error{"OpenAI Responses unexpected content-type"}
        , actual(std::move(actual))
    {
    }

    std::string expected = "text/event-stream";
    std::optional<std::string> actual;
};

/// Owns endpoint and trust configuration, never credentials. Windows hosts
/// must provision an explicit PEM CA bundle (e.g. app LocalState/ca.pem).
/// HTTPS only, with chain and strict SAN verification always enabled.
struct responses_transport_options
{
    std::string host = "api.openai.com";
    std::string service = "443";
    std::string target = "/v1/responses";
    std::string ca_file = {};
};

/// Reusable OpenAI Responses transport over NXT DNS/socket/HTTP/TLS/SSE.
/// Each call owns its request, socket and buffers, and borrows an observer
/// with awaitable text(std::string). The observer and this transport must
/// outlive the call. Cancellation drains network wishes before releasing
/// buffers or sockets. No credential loading, request logging, retries or
/// model substitution: the caller supplies request.api_key and model.
class responses_transport
{
public:
    explicit responses_transport(responses_transport_options options = {})
        : options_(std::move(options))
    {
    }

    template<typename Observer>
    nxtrt::task<responses::response_result> operator()(
        responses::openai_responses_request request,
        Observer & observer) const
    {
        nxtrt::throw_if_stop_requested();
        if (request.api_key.empty()
            || request.api_key.find_first_of("\r\n") != std::string::npos
            || request.api_key.find('\0') != std::string::npos)
            throw nxtrt::invalid_argument{"invalid OpenAI API key"};
        auto wire = responses::openai_responses_http_request(request);
        wire.host = options_.host.find(':') == std::string::npos
                        ? options_.host
                        : "[" + options_.host + "]";
        if (options_.service != "443" && options_.service != "https")
            wire.host += ":" + options_.service;
        wire.target = options_.target;
        for (auto & header : wire.headers)
            if (header.name == "Connection")
                header.value = "close";
        auto encodings = std::string{"gzip, deflate"};
#if defined(NXTRT_HAVE_ZSTD)
        encodings += ", zstd";
#endif
#if defined(NXTRT_HAVE_BROTLI)
        encodings += ", br";
#endif
        wire.headers.push_back({"Accept-Encoding", std::move(encodings)});
        auto request_text = nxt::http::serialize(wire);

        std::array<std::byte, 16 * 1024> tx{};
        std::array<std::byte, 64 * 1024> rx{}, tls_buffer{};
        auto socket = nxtrt::net::socket{
            co_await nxtrt::net::connect_tcp(
                options_.host, options_.service),
            tx,
            rx};
        auto tls = nxtrt::tls::tls13_client_session{socket, tls_buffer};
        co_await tls.handshake(options_.host, options_.ca_file);
        co_await tls.write_all(request_text);
        auto head = co_await nxtrt::http::read_response_head(tls);
        if (head.status < 200 || head.status >= 300)
            throw responses_http_error{head.status, head.reason};
        auto media_type = std::optional<std::string>{};
        if (auto value = nxtrt::http::header_value(head, "content-type"))
            media_type =
                nxtrt::http::trim_ascii(value->substr(0, value->find(';')));
        if (!media_type
            || !nxtrt::http::iequals(*media_type, "text/event-stream"))
            throw responses_content_type_error{std::move(media_type)};

        auto body = nxtrt::http::response_body_decoding_reader{
            tls, head, 1024 * 1024};
        auto events = nxtrt::http::sse_event_parser(body);
        auto decoder = responses::stream_decoder{};
        while (auto event = co_await events.take()) {
            if (auto delta =
                    co_await decoder.accept(event->type, event->data))
                co_await observer.text(std::move(*delta));
            if (decoder.completed)
                co_return decoder.finish();
        }
        co_return decoder.finish();
    }
private:
    responses_transport_options options_;
};

} // namespace nxtai
