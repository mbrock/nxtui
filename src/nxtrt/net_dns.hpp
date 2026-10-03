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
inline task<nxt::unique_fd> connect(resolved_address address)
{
    auto fd = nxt::unique_fd{::socket(
        address.family,
        address.socktype == 0 ? SOCK_STREAM : address.socktype,
        address.protocol)};
    if (fd.get() < 0)
        throw_errno("socket");

    set_close_on_exec(fd.get());
    set_tcp_no_delay(fd.get());
    co_await op::connect::from(
        fd.get(), address.sockaddr_ptr(), address.address_size);
    co_return std::move(fd);
}

/// Resolve HOST and SERVICE (a port number or service name) to stream
/// socket addresses of any family.
///
/// Uses a fresh `cares_resolver` when the build defines
/// `NXTRT_HAVE_CARES`, which suspends only the awaiting task. Otherwise it
/// uses `libc_resolver`, whose getaddrinfo(3) call blocks the whole deck
/// thread until it returns. Throws `runtime_error` when resolution fails.
inline task<std::vector<resolved_address>> resolve_tcp(
    std::string host,
    std::string service)
{
#if defined(NXTRT_HAVE_CARES)
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
/// to succeed stops the others, and if several succeed before they stop,
/// the earliest address in resolver order wins and the rest are closed.
/// If all fail, the failures are thrown together; no addresses throws
/// `runtime_error`.
///
/// @code
/// auto fd = co_await nxtrt::net::connect_tcp("example.com", "443");
/// @endcode
inline task<nxt::unique_fd> connect_tcp(
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
