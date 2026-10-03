#pragma once

#include "nxtrt/task.hpp"

#include <cstring>
#include <netdb.h>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <vector>

#if defined(NXTRT_HAVE_CARES)
#include <ares.h>

#include <chrono>
#include <poll.h>
#  include <array>
#endif

namespace nxtrt {

#if defined(NXTRT_HAVE_CARES)
#if defined(__GNUC__) || defined(__clang__)
#define NXT_RT_CARES_IGNORE_DEPRECATED_BEGIN \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define NXT_RT_CARES_IGNORE_DEPRECATED_END _Pragma("GCC diagnostic pop")
#else
#define NXT_RT_CARES_IGNORE_DEPRECATED_BEGIN
#define NXT_RT_CARES_IGNORE_DEPRECATED_END
#endif
#endif

/// One socket address from name resolution, with the family, socket type
/// and protocol to create a socket for it. Self-contained; copy freely.
struct resolved_address
{
    int family = AF_UNSPEC;
    int socktype = 0;
    int protocol = 0;
    sockaddr_storage address{};
    socklen_t address_size = 0;

    [[nodiscard]] sockaddr const * sockaddr_ptr() const noexcept
    {
        return reinterpret_cast<sockaddr const *>(&address);
    }
};

/// Copies the usable entries of a getaddrinfo(3) result list.
inline std::vector<resolved_address>
resolved_addresses_from(addrinfo * result)
{
    auto addresses = std::vector<resolved_address>{};
    for (auto * node = result; node != nullptr; node = node->ai_next) {
        if (node->ai_addr == nullptr
            || node->ai_addrlen > sizeof(sockaddr_storage))
            continue;

        auto address = resolved_address{
            .family = node->ai_family,
            .socktype = node->ai_socktype,
            .protocol = node->ai_protocol,
            .address = {},
            .address_size = static_cast<socklen_t>(node->ai_addrlen),
        };
        std::memcpy(&address.address, node->ai_addr, node->ai_addrlen);
        addresses.push_back(address);
    }
    return addresses;
}

/// Name resolution through the C library's getaddrinfo(3).
///
/// The call is synchronous: awaiting `getaddrinfo` blocks the deck thread,
/// and every other task on the deck, until the lookup finishes, and it
/// cannot be cancelled. It is the fallback when c-ares is not built in.
/// Throws `runtime_error` with the gai_strerror(3) text on failure.
class libc_resolver
{
public:
    task<std::vector<resolved_address>> getaddrinfo(
        std::string name,
        std::string service,
        int family = AF_UNSPEC,
        int socktype = SOCK_STREAM,
        int protocol = 0)
    {
        auto hints = addrinfo{};
        hints.ai_family = family;
        hints.ai_socktype = socktype;
        hints.ai_protocol = protocol;
        auto * result = static_cast<addrinfo *>(nullptr);
        auto rc = ::getaddrinfo(
            name.c_str(),
            service.empty() ? nullptr : service.c_str(),
            &hints,
            &result);
        auto cleanup = std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)>{
            result,
            ::freeaddrinfo};
        if (rc != 0)
            throw runtime_error{
                "getaddrinfo failed: " + std::string{::gai_strerror(rc)}};

        co_return resolved_addresses_from(result);
    }
};

#if defined(NXTRT_HAVE_CARES)

/// Asynchronous name resolution through c-ares, driven by runtime wishes.
///
/// Each resolver owns a configuration channel initialized from the system
/// resolver settings. Each lookup duplicates that channel and races poll
/// wishes for all its sockets against c-ares's timeout, without fd_set's
/// descriptor-number limit. Not copyable or movable; deck-confined.
/// Throws `runtime_error` with the c-ares error text on failure.
///
/// Cancellation drains the lookup's socket wishes, then destroys its
/// channel. Channel destruction invokes pending callbacks synchronously,
/// before the query state leaves the coroutine frame. Other lookups are
/// unaffected.
class cares_resolver
{
public:
    cares_resolver()
    {
        ensure_library();

        ares_channel_t * channel = nullptr;
        NXT_RT_CARES_IGNORE_DEPRECATED_BEGIN
        auto rc = ares_init(&channel);
        NXT_RT_CARES_IGNORE_DEPRECATED_END
        if (rc != ARES_SUCCESS)
            throw runtime_error{
                "ares_init failed: " + std::string{ares_strerror(rc)}};
        channel_.reset(channel);
    }

    cares_resolver(const cares_resolver &) = delete;
    cares_resolver & operator=(const cares_resolver &) = delete;
    cares_resolver(cares_resolver &&) = delete;
    cares_resolver & operator=(cares_resolver &&) = delete;

    task<std::vector<resolved_address>> getaddrinfo(
        std::string name,
        std::string service,
        int family = AF_UNSPEC,
        int socktype = SOCK_STREAM,
        int protocol = 0)
    {
        auto query = addrinfo_query{};
        // Each lookup owns its channel. Destroying it synchronously invokes
        // any pending callback before query leaves this coroutine frame,
        // including when drive_once is cancelled. Other lookups are
        // unaffected.
        ares_channel_t * duplicate = nullptr;
        NXT_RT_CARES_IGNORE_DEPRECATED_BEGIN
        auto rc = ares_dup(&duplicate, channel_.get());
        NXT_RT_CARES_IGNORE_DEPRECATED_END
        if (rc != ARES_SUCCESS)
            throw runtime_error{
                "ares_dup failed: " + std::string{ares_strerror(rc)}};
        auto channel =
            std::unique_ptr<ares_channel_t, channel_deleter>{duplicate};
        auto hints = ares_addrinfo_hints{
            .ai_flags = 0,
            .ai_family = family,
            .ai_socktype = socktype,
            .ai_protocol = protocol,
        };

        ares_getaddrinfo(
            channel.get(),
            name.c_str(),
            service.empty() ? nullptr : service.c_str(),
            &hints,
            complete_addrinfo,
            &query);

        while (!query.done)
            co_await drive_once(channel.get());

        if (query.error)
            std::rethrow_exception(query.error);
        if (query.status != ARES_SUCCESS)
            throw runtime_error{
                "ares_getaddrinfo failed: "
                + std::string{ares_strerror(query.status)}};

        co_return std::move(query.addresses);
    }

private:
    struct channel_deleter
    {
        void operator()(ares_channel_t * channel) const noexcept
        {
            if (channel != nullptr)
                ares_destroy(channel);
        }
    };

    struct library_guard
    {
        library_guard()
        {
            auto rc = ares_library_init(ARES_LIB_INIT_ALL);
            if (rc != ARES_SUCCESS)
                throw runtime_error{
                    "ares_library_init failed: "
                    + std::string{ares_strerror(rc)}};
        }

        ~library_guard()
        {
            ares_library_cleanup();
        }
    };

    struct addrinfo_query
    {
        bool done = false;
        int status = ARES_SUCCESS;
        std::vector<resolved_address> addresses;
        std::exception_ptr error;
    };

    static void ensure_library()
    {
        static auto guard = library_guard{};
        (void)guard;
    }

    static void complete_addrinfo(
        void * arg, int status, int, ares_addrinfo * result) noexcept
    {
        auto & query = *static_cast<addrinfo_query *>(arg);
        query.status = status;

        try {
            if (status == ARES_SUCCESS && result != nullptr) {
                for (auto * node = result->nodes; node != nullptr;
                     node = node->ai_next) {
                    if (node->ai_addr == nullptr
                        || node->ai_addrlen > sizeof(sockaddr_storage))
                        continue;

                    auto address = resolved_address{
                        .family = node->ai_family,
                        .socktype = node->ai_socktype,
                        .protocol = node->ai_protocol,
                        .address = {},
                        .address_size =
                            static_cast<socklen_t>(node->ai_addrlen),
                    };
                    std::memcpy(
                        &address.address, node->ai_addr, node->ai_addrlen);
                    query.addresses.push_back(address);
                }
            }
        } catch (...) {
            query.error = std::current_exception();
        }

        if (result != nullptr)
            ares_freeaddrinfo(result);

        query.done = true;
    }

    static task<pollfd> wait_socket(int fd, short events)
    {
        auto ready = co_await op::poll{fd, events};
        co_return pollfd{fd, events, static_cast<short>(ready)};
    }

    static task<pollfd> wait_timeout(std::chrono::nanoseconds duration)
    {
        co_await op::timeout::after(duration);
        co_return pollfd{ARES_SOCKET_BAD, 0, 0};
    }

    static task<> drive_once(ares_channel_t * channel)
    {
        // A single lookup has at most the A/AAAA sockets. Unlike ares_fds,
        // ares_getsock does not index a fixed-size fd_set by descriptor
        // value.
        auto sockets = std::array<ares_socket_t, ARES_GETSOCK_MAXNUM>{};
        NXT_RT_CARES_IGNORE_DEPRECATED_BEGIN
        auto bits = ares_getsock(channel, sockets.data(), sockets.size());
        NXT_RT_CARES_IGNORE_DEPRECATED_END
        auto waits = std::vector<task<pollfd>>{};
        for (auto i = 0; i < ARES_GETSOCK_MAXNUM; ++i) {
            auto events = short{0};
            if (ARES_GETSOCK_READABLE(bits, i))
                events |= POLLIN;
            if (ARES_GETSOCK_WRITABLE(bits, i))
                events |= POLLOUT;
            if (events != 0)
                waits.push_back(wait_socket(sockets[i], events));
        }
        auto timeout_storage = timeval{};
        auto * timeout = ares_timeout(channel, nullptr, &timeout_storage);
        if (timeout != nullptr)
            waits.push_back(wait_timeout(as_duration(*timeout)));
        auto ready = waits.empty()
                         ? pollfd{ARES_SOCKET_BAD, 0, 0}
                         : co_await wait_any_range(std::move(waits));
        auto has_error =
            (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        auto read_fd = ((ready.revents & POLLIN) || has_error)
                               && (ready.events & POLLIN)
                           ? ready.fd
                           : ARES_SOCKET_BAD;
        auto write_fd = ((ready.revents & POLLOUT) || has_error)
                                && (ready.events & POLLOUT)
                            ? ready.fd
                            : ARES_SOCKET_BAD;
        ares_process_fd(channel, read_fd, write_fd);
    }

    static std::chrono::nanoseconds as_duration(timeval value)
    {
        return std::chrono::seconds{value.tv_sec}
            + std::chrono::microseconds{value.tv_usec};
    }

    std::unique_ptr<ares_channel_t, channel_deleter> channel_;
};

#undef NXT_RT_CARES_IGNORE_DEPRECATED_BEGIN
#undef NXT_RT_CARES_IGNORE_DEPRECATED_END
#endif

} // namespace nxtrt
