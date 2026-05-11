#pragma once

#ifndef LIBCORO_FEATURE_NETWORKING
#define LIBCORO_FEATURE_NETWORKING
#define NXT_UNDEF_LIBCORO_FEATURE_NETWORKING
#endif
#ifndef LIBCORO_FEATURE_TLS
#define LIBCORO_FEATURE_TLS
#define NXT_UNDEF_LIBCORO_FEATURE_TLS
#endif
#include <coro/net/ip_address.hpp>
#include <coro/net/tcp/client.hpp>
#include <coro/net/tls/client.hpp>
#ifdef NXT_UNDEF_LIBCORO_FEATURE_NETWORKING
#undef LIBCORO_FEATURE_NETWORKING
#undef NXT_UNDEF_LIBCORO_FEATURE_NETWORKING
#endif
#ifdef NXT_UNDEF_LIBCORO_FEATURE_TLS
#undef LIBCORO_FEATURE_TLS
#undef NXT_UNDEF_LIBCORO_FEATURE_TLS
#endif

#include <nxtio/async.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace nxt::io::net {

/// Host and port to connect to.
struct endpoint
{
    /// DNS name or IP address.
    std::string_view host;
    /// TCP port.
    std::uint16_t port = 0;
};

/// DNS resolution result for a target endpoint.
struct resolved_target
{
    /// Candidate IP addresses returned by DNS.
    std::vector<coro::net::ip_address> addresses;
    /// Port copied from the target endpoint.
    std::uint16_t port = 0;
};

/// Async TCP transport with the read/write shape used by HTTP helpers.
class tcp_transport
{
public:
    /// Wrap an already-connected libcoro TCP client.
    explicit tcp_transport(coro::net::tcp::client client);

    /// Read bytes into `dst`.
    nxt::task<std::size_t> read_some(std::span<char> dst);
    /// Read bytes into `dst`, checking `stop` while waiting.
    nxt::task<std::size_t>
    read_some(std::span<char> dst, std::stop_token stop);
    /// Write all bytes to the socket.
    nxt::task<> write_all(std::string_view bytes);

private:
    coro::net::tcp::client client_;
};

/// Async TLS transport with the read/write shape used by HTTP helpers.
class tls_transport
{
public:
    /// Wrap an already-connected libcoro TLS client and context.
    tls_transport(
        std::shared_ptr<coro::net::tls::context> ctx,
        coro::net::tls::client client);

    /// Read decrypted bytes into `dst`.
    nxt::task<std::size_t> read_some(std::span<char> dst);
    /// Read decrypted bytes into `dst`, checking `stop` while waiting.
    nxt::task<std::size_t>
    read_some(std::span<char> dst, std::stop_token stop);
    /// Encrypt and write all bytes.
    nxt::task<> write_all(std::string_view bytes);
    /// Attempt an orderly TLS shutdown.
    nxt::task<> shutdown(std::chrono::seconds timeout = std::chrono::seconds{5});

private:
    std::shared_ptr<coro::net::tls::context> ctx_;
    coro::net::tls::client client_;
};

/// Resolve an endpoint using the scheduler-owned DNS resolver.
nxt::task<resolved_target> resolve_target(
    std::unique_ptr<nxt::scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

/// Resolve and open a TCP connection to an endpoint.
nxt::task<tcp_transport> connect_tcp(
    std::unique_ptr<nxt::scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

/// Resolve, open, and TLS-wrap a connection to an endpoint.
nxt::task<tls_transport> connect_tls(
    std::unique_ptr<nxt::scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

} // namespace nxt::io::net
