#include <nxtio/net.hpp>

#ifndef LIBCORO_FEATURE_NETWORKING
#define LIBCORO_FEATURE_NETWORKING
#define NXT_UNDEF_LIBCORO_FEATURE_NETWORKING
#endif
#ifndef LIBCORO_FEATURE_TLS
#define LIBCORO_FEATURE_TLS
#define NXT_UNDEF_LIBCORO_FEATURE_TLS
#endif
#include <coro/net/dns/resolver.hpp>
#include <coro/net/socket_address.hpp>
#include <coro/net/tls/context.hpp>
#ifdef NXT_UNDEF_LIBCORO_FEATURE_NETWORKING
#undef LIBCORO_FEATURE_NETWORKING
#undef NXT_UNDEF_LIBCORO_FEATURE_NETWORKING
#endif
#ifdef NXT_UNDEF_LIBCORO_FEATURE_TLS
#undef LIBCORO_FEATURE_TLS
#undef NXT_UNDEF_LIBCORO_FEATURE_TLS
#endif

#include <stdexcept>
#include <string>
#include <utility>

namespace nxt::io::net {

namespace {

constexpr auto cancellable_read_poll_interval =
    std::chrono::milliseconds{100};

void throw_if_stopped(std::stop_token stop)
{
    if (stop.stop_requested())
        throw std::runtime_error{"operation cancelled"};
}

} // namespace

tcp_transport::tcp_transport(coro::net::tcp::client client)
    : client_(std::move(client))
{
}

nxt::task<std::size_t> tcp_transport::read_some(std::span<char> dst)
{
    if (dst.empty())
        co_return 0;

    auto [status, bytes] = co_await client_.read_some(dst);
    if (status.is_ok())
        co_return bytes.size();
    if (status.is_closed())
        co_return 0;

    throw std::runtime_error{"read: " + status.message()};
}

nxt::task<std::size_t>
tcp_transport::read_some(std::span<char> dst, std::stop_token stop)
{
    if (dst.empty())
        co_return 0;

    while (true) {
        throw_if_stopped(stop);
        auto [status, bytes] = co_await client_.read_some(
            dst,
            cancellable_read_poll_interval);
        if (status.is_ok())
            co_return bytes.size();
        if (status.is_closed())
            co_return 0;
        if (status.is_timeout())
            continue;

        throw std::runtime_error{"read: " + status.message()};
    }
}

nxt::task<> tcp_transport::write_all(std::string_view bytes)
{
    auto buffer = std::span<const char>{bytes.data(), bytes.size()};
    auto [status, rest] = co_await client_.write_all(buffer);
    if (!status.is_ok()) {
        (void) rest;
        throw std::runtime_error{"write: " + status.message()};
    }
}

tls_transport::tls_transport(
    std::shared_ptr<coro::net::tls::context> ctx,
    coro::net::tls::client client)
    : ctx_(std::move(ctx))
    , client_(std::move(client))
{
}

nxt::task<std::size_t> tls_transport::read_some(std::span<char> dst)
{
    if (dst.empty())
        co_return 0;

    auto [status, bytes] = co_await client_.recv(dst);
    if (status == coro::net::tls::recv_status::ok)
        co_return bytes.size();
    if (status == coro::net::tls::recv_status::closed)
        co_return 0;

    throw std::runtime_error{
        "TLS read: " + coro::net::tls::to_string(status)};
}

nxt::task<std::size_t>
tls_transport::read_some(std::span<char> dst, std::stop_token stop)
{
    if (dst.empty())
        co_return 0;

    while (true) {
        throw_if_stopped(stop);
        auto [status, bytes] = co_await client_.recv(
            dst,
            cancellable_read_poll_interval);
        if (status == coro::net::tls::recv_status::ok)
            co_return bytes.size();
        if (status == coro::net::tls::recv_status::closed)
            co_return 0;
        if (status == coro::net::tls::recv_status::timeout)
            continue;
        if (status == coro::net::tls::recv_status::cancelled)
            throw std::runtime_error{"operation cancelled"};

        throw std::runtime_error{
            "TLS read: " + coro::net::tls::to_string(status)};
    }
}

nxt::task<> tls_transport::write_all(std::string_view bytes)
{
    auto remaining = std::span<const char>{bytes.data(), bytes.size()};
    while (!remaining.empty()) {
        auto [status, rest] = co_await client_.send(remaining);
        if (status == coro::net::tls::send_status::ok) {
            if (rest.size() == remaining.size())
                throw std::runtime_error{"TLS write made no progress"};
            remaining = rest;
            continue;
        }

        throw std::runtime_error{
            "TLS write: " + coro::net::tls::to_string(status)};
    }
}

nxt::task<> tls_transport::shutdown(std::chrono::seconds timeout)
{
    co_await client_.shutdown(timeout);
}

nxt::task<resolved_target> resolve_target(
    std::unique_ptr<nxt::io_scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout)
{
    auto resolver = coro::net::dns::resolver<nxt::io_scheduler>{
        sched,
        timeout,
    };
    auto dns = co_await resolver.host_by_name(
        coro::net::hostname{std::string{target.host}});

    if (dns->status() != coro::net::dns::status::complete
        || dns->ip_addresses().empty()) {
        throw std::runtime_error{"DNS lookup failed"};
    }

    co_return resolved_target{
        .addresses = dns->ip_addresses(),
        .port = target.port,
    };
}

nxt::task<tcp_transport> connect_tcp(
    std::unique_ptr<nxt::io_scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout)
{
    auto resolved = co_await resolve_target(sched, target, timeout);
    auto last_status = coro::net::connect_status::error;

    for (auto const & address : resolved.addresses) {
        auto socket_endpoint =
            coro::net::socket_address{address, resolved.port};
        auto client = coro::net::tcp::client{
            sched,
            socket_endpoint,
        };
        last_status = co_await client.connect();
        if (last_status == coro::net::connect_status::connected)
            co_return tcp_transport{std::move(client)};
    }

    throw std::runtime_error{
        "connect: " + coro::net::to_string(last_status)};
}

nxt::task<tls_transport> connect_tls(
    std::unique_ptr<nxt::io_scheduler> & sched,
    endpoint target,
    std::chrono::milliseconds timeout)
{
    auto resolved = co_await resolve_target(sched, target, timeout);
    auto last_status = coro::net::tls::connection_status::error;
    auto ctx = std::make_shared<coro::net::tls::context>(
        coro::net::tls::verify_peer_t::yes);

    for (auto const & address : resolved.addresses) {
        auto socket_endpoint =
            coro::net::socket_address{address, resolved.port};
        auto client = coro::net::tls::client{
            sched,
            ctx,
            socket_endpoint,
            std::string{target.host},
        };
        last_status = co_await client.connect(timeout);
        if (last_status == coro::net::tls::connection_status::connected)
            co_return tls_transport{ctx, std::move(client)};
    }

    throw std::runtime_error{
        "TLS connect: " + coro::net::tls::to_string(last_status)};
}

} // namespace nxt::io::net
