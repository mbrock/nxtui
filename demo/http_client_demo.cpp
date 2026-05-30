// A small curl-like client built entirely on the nxt byte_reader tower:
//
//   socket_source -> tls13_client_session -> response_body_reader -> stdout
//                    (TLS 1.3, handmade)     (de-chunk + decompress)
//
// It advertises every content encoding the build supports and lets the
// response_body_reader transparently inflate whatever the server returns, so a
// single drain loop handles identity, chunked, gzip, deflate, zstd, and brotli
// bodies. Response metadata goes to stderr; the decoded body goes to stdout,
// so it pipes like curl and is easy to watch under `strace`.

#include "nxtrt/task.hpp"
#include <nxtrt/buffers.hpp>
#include <nxtrt/http.hpp>
#include <nxtrt/net_dns.hpp>
#include <nxtrt/tls.hpp>

#if defined(__linux__)
#  include <nxtrt/uring_wand.hpp>
#else
#  include <nxtrt/kqueue_wand.hpp>
#endif

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

// Every encoding this build can decode, so the server is free to pick its best.
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

std::string build_request(const nxtrt::http::url & url)
{
    auto request = nxtrt::http::request{
        .method = "GET",
        .target = url.target,
        .host = nxtrt::http::host_header(url),
        .headers =
            {
                {"User-Agent", "nxt-curl/0"},
                {"Accept", "*/*"},
                {"Accept-Encoding", accept_encoding_header()},
            },
        .body = {},
    };
    return nxtrt::http::serialize(request);
}

void report_head(const nxtrt::http::response_head & head)
{
    std::cerr << "< " << head.version << ' ' << head.status << ' '
              << head.reason << '\n';
    for (auto const & header : head.headers)
        std::cerr << "< " << header.name << ": " << header.value << '\n';
    std::cerr << '\n';
}

// Drain a fully-assembled body reader (already de-chunked and decompressed) to
// stdout, returning the number of decoded bytes.
nxtrt::task<std::size_t> drain_to_stdout(nxtrt::byte_reader & body)
{
    auto out = nxtrt::standard_output(64 * 1024);
    co_return co_await nxtrt::stream_all(body, out);
}

nxtrt::task<void> fetch(nxtrt::http::url url)
{
    auto request_text = build_request(url);
    std::cerr << "> GET " << url.target << " HTTP/1.1 (" << url.host << ":"
              << url.port << (url.tls ? ", TLS 1.3)\n" : ")\n");

    auto socket = co_await nxtrt::net::connect_tcp(url.host, url.port);
    auto sink = nxtrt::socket_sink{socket.get(), 0, std::size_t{16 * 1024}};
    auto source =
        nxtrt::socket_source{socket.get(), 0, std::size_t{64 * 1024}};

    // The transport reader is the TLS session for https, or the raw socket for
    // http. Either way it is just a byte_reader to everything above.
    auto tls = std::optional<nxtrt::tls::tls13_client_session>{};
    auto * transport = static_cast<nxtrt::byte_reader *>(&source);
    if (url.tls) {
        tls.emplace(source, sink, std::size_t{64 * 1024});
        co_await tls->handshake(url.host);
        co_await tls->write_all(request_text);
        transport = &*tls;
    } else {
        co_await sink.write(request_text);
        co_await sink.flush();
    }

    auto head = co_await nxtrt::http::read_response_head(*transport);
    report_head(head);

    auto body = nxtrt::http::response_body_decoding_reader(*transport, head);
    auto decoded = co_await drain_to_stdout(body);

    auto encoding = nxtrt::http::header_value(head, "content-encoding");
    std::cerr << "\n* decoded " << decoded << " body bytes (content-encoding: "
              << (encoding ? *encoding : std::string_view{"identity"})
              << ")\n";
}

} // namespace

int main(int argc, char ** argv)
try {
    auto url = nxtrt::http::parse_url(
        argc > 1 ? std::string_view{argv[1]}
                 : std::string_view{"https://less.rest/"});

#if defined(__linux__)
    nxtrt::run(
        [url = std::move(url)]() mutable { return fetch(std::move(url)); });
#elif NXT_RT_HAS_KQUEUE
    nxtrt::run_with_kqueue(
        [url = std::move(url)]() mutable { return fetch(std::move(url)); });
#else
    static_assert(NXT_RT_HAS_KQUEUE, "http demo needs a runtime wand");
#endif

    return 0;
} catch (std::exception const & error) {
    std::cerr << "nxt-curl: " << error.what() << '\n';
    return 1;
}
