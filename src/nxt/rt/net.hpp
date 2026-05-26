#pragma once

#include "nxt/rt/cares.hpp"
#include "nxt/rt/task.hpp"
#include "nxt/unique-fd.hpp"

#include <cerrno>
#include <fcntl.h>
#include <ranges>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace nxt::rt::net {

inline void set_close_on_exec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

inline task<nxt::unique_fd> connect(resolved_address address)
{
    auto fd = nxt::unique_fd{::socket(
        address.family,
        address.socktype == 0 ? SOCK_STREAM : address.socktype,
        address.protocol)};
    if (fd.get() < 0)
        throw runtime_error{
            "socket: "
            + std::string{std::generic_category().message(errno)}};

    set_close_on_exec(fd.get());
    co_await op::connect::from(
        fd.get(), address.sockaddr_ptr(), address.address_size);
    co_return std::move(fd);
}

inline task<nxt::unique_fd> connect_tcp(
    std::string host,
    std::string service)
{
    auto resolver = cares_resolver{};
    auto addresses = co_await resolver.getaddrinfo(host, service);
    if (addresses.empty())
        throw runtime_error{"no addresses resolved for " + host};

    co_return co_await wait_any_range(
        addresses | std::views::transform([](auto const & address) {
            return connect(address);
        }));
}

} // namespace nxt::rt::net
