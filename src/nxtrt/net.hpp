#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/task.hpp"
#if defined(_WIN32)
#  include "nxtrt/wand/iocp.hpp"
#else
#  include "nxt/unique-fd.hpp"

#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#endif
#include <cerrno>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

/**
 * @namespace nxtrt::net
 * TCP sockets for runtime tasks: listen, accept, resolve and connect.
 *
 * `connect_tcp(host, service)` (in `nxtrt/net_dns.hpp`) resolves a name and
 * connects; `listen_tcp_loopback` and `accept` serve. All return owned
 * sockets, with close-on-exec on POSIX and TCP_NODELAY on connections. Wrap
 * a connected descriptor in `socket` to get a buffered feed and sink for
 * protocol code such as `nxtrt::http` and `nxtrt::tls`. Connects and
 * accepts are wishes that suspend only the awaiting task; Windows uses
 * asynchronous native DNS, POSIX uses c-ares when available and blocking
 * getaddrinfo otherwise. IOCP connects attach themselves; attach a listener
 * once to its deck's port with `attach_socket` before its first accept.
 * Accepted sockets are already attached to that same port.
 */
namespace nxtrt::net {

#if defined(_WIN32)
/// Owned Winsock SOCKET, never a CRT descriptor. Pending wishes must drain
/// before destruction. Winsock initialization is process-wide and
/// automatic.
class unique_socket
{
public:
    explicit unique_socket(
        socket_handle fd = invalid_socket_handle) noexcept
        : fd_(fd)
    {
    }

    ~unique_socket()
    {
        reset();
    }

    unique_socket(unique_socket const &) = delete;
    unique_socket & operator=(unique_socket const &) = delete;

    unique_socket(unique_socket && other) noexcept
        : fd_(other.release())
    {
    }

    unique_socket & operator=(unique_socket && other) noexcept
    {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    socket_handle get() const noexcept
    {
        return fd_;
    }

    socket_handle release() noexcept
    {
        return std::exchange(fd_, invalid_socket_handle);
    }

    void reset(socket_handle fd = invalid_socket_handle) noexcept
    {
        if (fd_ != invalid_socket_handle)
            closesocket(fd_);
        fd_ = fd;
    }
private:
    socket_handle fd_;
};

inline void ensure_winsock()
{
    struct startup
    {
        startup()
        {
            WSADATA data{};
            auto error = WSAStartup(MAKEWORD(2, 2), &data);
            if (error)
                throw std::system_error{error, std::system_category()};
        }

        ~startup()
        {
            WSACleanup();
        }
    };

    static startup guard;
}
#else
using unique_socket = nxt::unique_fd;
#endif

/// An owned connected socket with a buffered input feed and output sink.
///
/// The transmit and receive buffers are borrowed and must outlive the
/// socket; FLAGS are passed to every send(2) and recv(2) (for example
/// `MSG_NOSIGNAL`). The descriptor closes when the socket is destroyed, so
/// no operation on it may still be pending then.
class socket
{
public:
    socket(
        unique_socket fd,
        std::span<std::byte> tx_buffer,
        std::span<std::byte> rx_buffer,
        int flags = 0)
        : fd_(std::move(fd))
        , input_(fd_.get(), rx_buffer, flags)
        , output_(fd_.get(), tx_buffer, flags)
    {
    }

    [[nodiscard]] socket_handle fd() const noexcept
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
    unique_socket fd_;
    socket_source input_;
    socket_sink output_;
};

/// Throws `runtime_error` with WHAT and the message for the current errno.
inline void throw_errno(std::string_view what)
{
#if defined(_WIN32)
    throw std::system_error{
        WSAGetLastError(), std::system_category(), std::string{what}};
#else
    throw runtime_error{
        std::string{what} + ": "
        + std::string{std::generic_category().message(errno)}};
#endif
}

inline void set_close_on_exec(socket_handle fd)
{
#if !defined(_WIN32)
    auto flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
#else
    (void) fd;
#endif
}

inline void set_reuse_address(socket_handle fd)
{
    auto yes = int{1};
    if (::setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<const char *>(&yes),
            sizeof(yes))
        != 0)
        throw_errno("setsockopt(SO_REUSEADDR)");
}

inline void set_tcp_no_delay(socket_handle fd)
{
    auto yes = int{1};
    (void) ::setsockopt(
        fd,
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<const char *>(&yes),
        sizeof(yes));
}

inline unique_socket
make_socket(int family, int type = SOCK_STREAM, int protocol = 0)
{
#if defined(_WIN32)
    ensure_winsock();
    auto fd = unique_socket{WSASocketW(
        family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED)};
#else
    auto fd = unique_socket{::socket(family, type, protocol)};
#endif
    if (fd.get() == invalid_socket_handle)
        throw_errno("socket");
    set_close_on_exec(fd.get());
    return fd;
}

/// Attach a fresh socket before its first wish on the current deck.
/// Accepted sockets already belong to that port; do not attach them again.
inline void attach_socket(socket_handle fd)
{
#if defined(_WIN32)
    auto * env = current_env();
    auto * backend =
        env && env->current_deck
            ? dynamic_cast<iocp_wand *>(env->current_deck->current_wand())
            : nullptr;
    if (!backend)
        throw runtime_error{"Winsock I/O requires an IOCP deck"};
    backend->attach(fd);
#else
    (void) fd;
#endif
}

/// Return the bound IPv4 address for a socket.
inline sockaddr_in socket_address(socket_handle fd)
{
    auto address = sockaddr_in{};
    auto size = socket_length{sizeof(address)};
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size)
        != 0)
        throw_errno("getsockname");
    return address;
}

/// Create a TCP listener bound to 127.0.0.1, with SO_REUSEADDR and
/// close-on-exec.
///
/// The default port of zero asks the kernel to choose an ephemeral port;
/// read it back with `socket_address`. Throws `runtime_error` on failure.
inline unique_socket
listen_tcp_loopback(std::uint16_t port = 0, int backlog = SOMAXCONN)
{
    auto fd = make_socket(AF_INET);

    set_close_on_exec(fd.get());
    set_reuse_address(fd.get());

    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (::bind(
            fd.get(),
            reinterpret_cast<sockaddr *>(&address),
            sizeof(address))
        != 0)
        throw_errno("bind");
    if (::listen(fd.get(), backlog) != 0)
        throw_errno("listen");

    return fd;
}

/// Accept one TCP connection and return it as an owned descriptor, with
/// close-on-exec (where SOCK_CLOEXEC exists) and TCP_NODELAY.
///
/// Suspends the awaiting task until a connection arrives; cancellation
/// cancels the accept wish.
inline task<unique_socket> accept(socket_handle listener)
{
    auto flags = int{0};
#ifdef SOCK_CLOEXEC
    flags |= SOCK_CLOEXEC;
#endif
    auto fd = unique_socket{co_await op::accept{listener, flags}};
    if (fd.get() == invalid_socket_handle)
        throw runtime_error{"accept returned invalid fd"};
    set_tcp_no_delay(fd.get());
    co_return std::move(fd);
}

/// Connect a new TCP socket to an IPv4 address, suspending until the
/// connection is established. Errors from the wand propagate as
/// exceptions; the socket is closed on failure.
inline task<unique_socket> connect(sockaddr_in address)
{
    auto fd = make_socket(AF_INET);

    set_close_on_exec(fd.get());
    set_tcp_no_delay(fd.get());
    attach_socket(fd.get());
    co_await op::connect::from(
        fd.get(),
        reinterpret_cast<sockaddr const *>(&address),
        sizeof(address));
    co_return std::move(fd);
}

} // namespace nxtrt::net
