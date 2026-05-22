#include "nxt/rt/task.hpp"
#include <nxt/rt/buffers.hpp>
#include <nxt/rt/cares.hpp>
#include <nxt/rt/http.hpp>
#include <nxt/unique-fd.hpp>

#if defined(__linux__)
#include <nxt/rt/uring_wand.hpp>
#else
#include <nxt/rt/kqueue_wand.hpp>
#endif

#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <span>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void set_close_on_exec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

nxt::rt::task<nxt::unique_fd>
connect_tcp(std::string host, std::string port)
{
    auto resolver = nxt::rt::cares_resolver{};
    auto addresses = co_await resolver.getaddrinfo(host, port);
    auto failures = std::vector<std::string>{};

    for (auto const & address : addresses) {
        auto fd = nxt::unique_fd{::socket(
            address.family,
            address.socktype == 0 ? SOCK_STREAM : address.socktype,
            address.protocol)};
        if (fd.get() < 0) {
            failures.push_back(
                "socket: " + std::string{std::generic_category().message(
                                errno)});
            continue;
        }
        set_close_on_exec(fd.get());

        try {
            co_await nxt::rt::op::connect::from(
                fd.get(), address.sockaddr_ptr(), address.address_size);
            co_return std::move(fd);
        } catch (std::exception const & error) {
            failures.push_back(error.what());
        }
    }

    if (failures.empty())
        throw nxt::rt::runtime_error{"no addresses resolved for " + host};

    throw nxt::rt::runtime_error{
        "connect failed for " + host + ":" + port + ": " + failures.back()};
}

nxt::rt::task<void> fetch(nxt::rt::http::url url)
{
    if (url.tls)
        throw nxt::rt::runtime_error{
            "ng-http-client-demo does not speak TLS yet"};

    auto socket = co_await connect_tcp(url.host, url.port);
    auto request = nxt::rt::http::request{
        .method = "GET",
        .target = url.target,
        .host = nxt::rt::http::host_header(url),
        .headers =
            {
                {"User-Agent", "nxt-ng-http-demo/0"},
                {"Accept", "*/*"},
            },
        .body = {},
    };

    auto socket_output = nxt::rt::byte_writer<nxt::rt::socket_sink>{
        nxt::rt::socket_sink{socket.get()},
        4096,
    };
    co_await socket_output.write_all(nxt::rt::http::serialize(request));

    auto source = nxt::rt::socket_source{socket.get()};
    auto input_storage = std::vector<std::byte>(64 * 1024);
    auto reader = nxt::rt::byte_reader<nxt::rt::socket_source>{
        source,
        std::span{input_storage},
    };

    auto head = co_await nxt::rt::http::read_response_head(reader);
    std::cerr << head.version << ' ' << head.status << ' ' << head.reason
              << "\n";
    for (auto const & header : head.headers)
        std::cerr << header.name << ": " << header.value << "\n";
    std::cerr << "\n";

    auto stdout_writer = nxt::rt::standard_output_writer(64 * 1024);
    auto body = nxt::rt::http::read_response_body(reader, head);
    while (auto chunk = co_await body.next())
        co_await stdout_writer.write(*chunk);
    co_await stdout_writer.flush();
}

} // namespace

int main(int argc, char ** argv)
try {
    auto url = nxt::rt::http::parse_url(
        argc > 1 ? std::string_view{argv[1]}
                 : std::string_view{"http://example.com/"});

#if defined(__linux__)
    nxt::rt::run([url = std::move(url)]() mutable {
        return fetch(std::move(url));
    });
#elif NXT_RT_HAS_KQUEUE
    nxt::rt::run_with_kqueue([url = std::move(url)]() mutable {
        return fetch(std::move(url));
    });
#else
    static_assert(NXT_RT_HAS_KQUEUE, "ng http demo needs a runtime wand");
#endif

    return 0;
} catch (std::exception const & error) {
    std::cerr << "ng-http-client-demo: " << error.what() << '\n';
    return 1;
}
