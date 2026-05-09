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

struct endpoint
{
    std::string_view host;
    std::uint16_t port = 0;
};

struct resolved_target
{
    std::vector<coro::net::ip_address> addresses;
    std::uint16_t port = 0;
};

class tcp_transport
{
public:
    explicit tcp_transport(coro::net::tcp::client client);

    nxt::task<std::size_t> read_some(std::span<char> dst);
    nxt::task<std::size_t>
    read_some(std::span<char> dst, std::stop_token stop);
    nxt::task<> write_all(std::string_view bytes);

private:
    coro::net::tcp::client client_;
};

class tls_transport
{
public:
    tls_transport(
        std::shared_ptr<coro::net::tls::context> ctx,
        coro::net::tls::client client);

    nxt::task<std::size_t> read_some(std::span<char> dst);
    nxt::task<std::size_t>
    read_some(std::span<char> dst, std::stop_token stop);
    nxt::task<> write_all(std::string_view bytes);
    nxt::task<> shutdown(std::chrono::seconds timeout = std::chrono::seconds{5});

private:
    std::shared_ptr<coro::net::tls::context> ctx_;
    coro::net::tls::client client_;
};

nxt::task<resolved_target> resolve_target(
    std::unique_ptr<nxt::io_scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

nxt::task<tcp_transport> connect_tcp(
    std::unique_ptr<nxt::io_scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

nxt::task<tls_transport> connect_tls(
    std::unique_ptr<nxt::io_scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

} // namespace nxt::io::net
