#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/task.hpp"
#include "nxt/unique-fd.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace nxtrt::net {

class socket
{
public:
    socket(
        nxt::unique_fd fd,
        std::span<std::byte> tx_buffer,
        std::span<std::byte> rx_buffer,
        int flags = 0)
        : fd_(std::move(fd))
        , input_(fd_.get(), rx_buffer, flags)
        , output_(fd_.get(), tx_buffer, flags)
    {
    }

    [[nodiscard]] int fd() const noexcept
    {
        return fd_.get();
    }

    [[nodiscard]] socket_source & input() noexcept
    {
        return input_;
    }

    [[nodiscard]] socket_sink & output() noexcept
    {
        return output_;
    }

private:
    nxt::unique_fd fd_;
    socket_source input_;
    socket_sink output_;
};

inline void throw_errno(std::string_view what)
{
    throw runtime_error{
        std::string{what}
        + ": "
        + std::string{std::generic_category().message(errno)}};
}

inline void set_close_on_exec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

inline void set_reuse_address(int fd)
{
    auto yes = int{1};
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0)
        throw_errno("setsockopt(SO_REUSEADDR)");
}

inline void set_tcp_no_delay(int fd)
{
    auto yes = int{1};
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

/// Return the bound IPv4 address for a socket.
inline sockaddr_in socket_address(int fd)
{
    auto address = sockaddr_in{};
    auto size = socklen_t{sizeof(address)};
    if (::getsockname(
            fd,
            reinterpret_cast<sockaddr *>(&address),
            &size) != 0)
        throw_errno("getsockname");
    return address;
}

/// Create a TCP listener bound to loopback.
///
/// The default port of zero asks the kernel to choose an ephemeral port.
inline nxt::unique_fd listen_tcp_loopback(
    std::uint16_t port = 0,
    int backlog = SOMAXCONN)
{
    auto fd = nxt::unique_fd{::socket(AF_INET, SOCK_STREAM, 0)};
    if (fd.get() < 0)
        throw_errno("socket");

    set_close_on_exec(fd.get());
    set_reuse_address(fd.get());

    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (::bind(
            fd.get(),
            reinterpret_cast<sockaddr *>(&address),
            sizeof(address)) != 0)
        throw_errno("bind");
    if (::listen(fd.get(), backlog) != 0)
        throw_errno("listen");

    return fd;
}

/// Accept one TCP connection and return it as an owned descriptor.
inline task<nxt::unique_fd> accept(int listener)
{
    auto flags = int{0};
#ifdef SOCK_CLOEXEC
    flags |= SOCK_CLOEXEC;
#endif
    auto fd = nxt::unique_fd{
        co_await op::accept{
            .fd = listener,
            .flags = flags,
        }};
    if (fd.get() < 0)
        throw runtime_error{"accept returned invalid fd"};
    set_tcp_no_delay(fd.get());
    co_return std::move(fd);
}

/// Connect a TCP socket to an IPv4 address.
inline task<nxt::unique_fd> connect(sockaddr_in address)
{
    auto fd = nxt::unique_fd{::socket(AF_INET, SOCK_STREAM, 0)};
    if (fd.get() < 0)
        throw_errno("socket");

    set_close_on_exec(fd.get());
    set_tcp_no_delay(fd.get());
    co_await op::connect::from(
        fd.get(),
        reinterpret_cast<sockaddr const *>(&address),
        sizeof(address));
    co_return std::move(fd);
}

} // namespace nxtrt::net
