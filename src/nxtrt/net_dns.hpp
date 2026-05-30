#pragma once

#include "nxtrt/cares.hpp"
#include "nxtrt/net.hpp"

#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace nxtrt::net {

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
