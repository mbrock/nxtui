#include <nxtio/async.hpp>
#include <nxtio/buffers.hpp>
#include <nxtio/http.hpp>
#include <nxtio/net.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace {

template<typename Transport>
nxt::task<>
fetch_over(Transport & transport, nxt::io::http::url const & url)
{
    auto host_header = nxt::io::http::is_default_port(url)
                           ? url.host
                           : url.host + ":" + url.port;
    auto request = std::string{
        "GET " + url.target + " HTTP/1.1\r\n"
        "Host: " + host_header + "\r\n"
        "User-Agent: nxthttp/0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n"};

    co_await transport.write_all(request);

    auto read_buffer = std::array<std::byte, 16 * 1024>{};
    auto reader = nxt::io::byte_reader{transport, std::span{read_buffer}};
    auto head = co_await reader.take_until("\r\n\r\n");
    auto response = nxt::io::http::parse_response_head(head);

    std::cerr << nxt::io::http::response_status_text(response) << '\n';

    auto on_chunk = [](std::span<const std::byte> chunk) -> nxt::task<> {
        auto text = nxt::io::http::as_text(chunk);
        std::cout.write(
            text.data(), static_cast<std::streamsize>(text.size()));
        std::cout.flush();
        co_return;
    };

    if (nxt::io::http::response_is_chunked(response)) {
        co_await nxt::io::http::read_chunked(reader, on_chunk);
    } else if (auto content_length =
                   nxt::io::http::response_content_length(response)) {
        co_await nxt::io::http::read_content_length(
            reader,
            *content_length,
            on_chunk);
    } else {
        co_await nxt::io::http::read_until_eof(reader, on_chunk);
    }
}

nxt::task<>
fetch(std::unique_ptr<nxt::io_scheduler> & sched, nxt::io::http::url url)
{
    if (url.tls) {
        auto transport = co_await nxt::io::net::connect_tls(
            sched,
            nxt::io::net::endpoint{
                .host = url.host,
                .port = nxt::io::http::parse_port(url.port),
            });
        co_await fetch_over(transport, url);
        co_await transport.shutdown();
        co_return;
    }

    auto transport = co_await nxt::io::net::connect_tcp(
        sched,
        nxt::io::net::endpoint{
            .host = url.host,
            .port = nxt::io::http::parse_port(url.port),
        });
    co_await fetch_over(transport, url);
}

} // namespace

int main(int argc, char ** argv)
{
    if (argc != 2) {
        std::cerr << "usage: nxthttp http[s]://host[:port]/path\n";
        return EXIT_FAILURE;
    }

    try {
        auto url = nxt::io::http::parse_url(argv[1]);
        auto sched = nxt::io_scheduler::make_unique(
            nxt::io_scheduler::options{
                .execution_strategy = nxt::io_scheduler::
                    execution_strategy_t::process_tasks_inline,
            });
        nxt::sync_wait(sched->schedule(fetch(sched, std::move(url))));
    } catch (const std::exception & e) {
        std::cerr << "nxthttp: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
