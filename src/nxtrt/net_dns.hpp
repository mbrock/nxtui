#pragma once

#include "nxtrt/cares.hpp"
#include "nxtrt/net.hpp"

#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace nxtrt::net {

/// Connect a new socket of ADDRESS's family and protocol (a stream socket
/// if it names no type), with close-on-exec and TCP_NODELAY.
inline task<unique_socket> connect(resolved_address address)
{
    auto fd = make_socket(
        address.family,
        address.socktype == 0 ? SOCK_STREAM : address.socktype,
        address.protocol);

    set_close_on_exec(fd.get());
    set_tcp_no_delay(fd.get());
    attach_socket(fd.get());
    co_await op::connect::from(
        fd.get(), address.sockaddr_ptr(), address.address_size);
    co_return std::move(fd);
}

/// Resolve HOST and SERVICE (a port number or service name) to stream
/// socket addresses of any family.
///
/// Windows uses native `windows_resolver`; POSIX uses a fresh
/// `cares_resolver` when the build defines `NXTRT_HAVE_CARES`.
/// Both suspend only the awaiting task. Otherwise it
/// uses `libc_resolver`, whose getaddrinfo(3) call blocks the whole deck
/// thread until it returns. Native Windows errors throw `system_error`;
/// POSIX errors throw `runtime_error`.
inline task<std::vector<resolved_address>> resolve_tcp(
    std::string host,
    std::string service)
{
#if defined(_WIN32)
    auto resolver = windows_resolver{};
#elif defined(NXTRT_HAVE_CARES)
    auto resolver = cares_resolver{};
#else
    auto resolver = libc_resolver{};
#endif
    co_return co_await resolver.getaddrinfo(
        std::move(host),
        std::move(service));
}

/// Resolve HOST and SERVICE and connect to one of the addresses.
///
/// Connection attempts to every resolved address start at once. The first
/// to succeed wins and stops the others. All attempts drain before
/// returning; any other sockets that connect despite cancellation are
/// closed. If all fail, the failures are thrown together; no addresses
/// throws `runtime_error`.
///
/// @code
/// auto fd = co_await nxtrt::net::connect_tcp("example.com", "443");
/// @endcode
inline task<unique_socket> connect_tcp(
    std::string host,
    std::string service)
{
    auto addresses = co_await resolve_tcp(host, service);
    if (addresses.empty())
        throw runtime_error{"no addresses resolved for " + host};

    co_return co_await wait_any_range(
        addresses | std::views::transform([](auto const & address) {
            return connect(address);
        }));
}

} // namespace nxtrt::net
