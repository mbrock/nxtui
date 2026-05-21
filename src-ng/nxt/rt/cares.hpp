#pragma once

#include "nxt/rt/task.hpp"

#include <ares.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace nxt::rt {

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

class cares_resolver
{
public:
    cares_resolver()
    {
        ensure_library();

        ares_channel_t * channel = nullptr;
        auto rc = ares_init(&channel);
        if (rc != ARES_SUCCESS)
            throw std::runtime_error{
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
        auto hints = ares_addrinfo_hints{
            .ai_flags = 0,
            .ai_family = family,
            .ai_socktype = socktype,
            .ai_protocol = protocol,
        };

        ares_getaddrinfo(
            channel_.get(),
            name.c_str(),
            service.empty() ? nullptr : service.c_str(),
            &hints,
            complete_addrinfo,
            &query);

        while (!query.done)
            co_await drive_once();

        if (query.status != ARES_SUCCESS)
            throw std::runtime_error{
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
                throw std::runtime_error{
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
    };

    static void ensure_library()
    {
        static auto guard = library_guard{};
        (void)guard;
    }

    static void complete_addrinfo(
        void * arg,
        int status,
        int,
        ares_addrinfo * result)
    {
        auto & query = *static_cast<addrinfo_query *>(arg);
        query.status = status;

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
                    .address_size = static_cast<socklen_t>(node->ai_addrlen),
                };
                std::memcpy(&address.address, node->ai_addr, node->ai_addrlen);
                query.addresses.push_back(address);
            }
        }

        if (result != nullptr)
            ares_freeaddrinfo(result);

        query.done = true;
    }

    task<> drive_once()
    {
        auto read_fds = fd_set{};
        auto write_fds = fd_set{};
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        auto timeout_storage = timeval{};
        auto * timeout = ares_timeout(channel_.get(), nullptr, &timeout_storage);
        auto nfds = ares_fds(channel_.get(), &read_fds, &write_fds);
        if (nfds == 0) {
            if (timeout != nullptr)
                co_await timeout_wish::after(as_duration(*timeout));
            ares_process_fd(
                channel_.get(),
                ARES_SOCKET_BAD,
                ARES_SOCKET_BAD);
            co_return;
        }

        for (auto fd = 0; fd < nfds; ++fd) {
            auto events = short{0};
            if (FD_ISSET(fd, &read_fds))
                events |= POLLIN;
            if (FD_ISSET(fd, &write_fds))
                events |= POLLOUT;
            if (events == 0)
                continue;

            auto result = timeout != nullptr
                ? co_await poll_until_wish::after(
                    fd,
                    events,
                    as_duration(*timeout))
                : poll_until_result{
                    .events = co_await poll_wish{
                        .fd = fd,
                        .events = events,
                    },
                    .timed_out = false,
                };
            if (result.timed_out) {
                ares_process_fd(
                    channel_.get(),
                    ARES_SOCKET_BAD,
                    ARES_SOCKET_BAD);
                co_return;
            }

            auto revents = result.events;
            auto has_error = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            auto read_fd = ((revents & POLLIN) || has_error)
                    && FD_ISSET(fd, &read_fds)
                ? fd
                : ARES_SOCKET_BAD;
            auto write_fd = ((revents & POLLOUT) || has_error)
                    && FD_ISSET(fd, &write_fds)
                ? fd
                : ARES_SOCKET_BAD;
            ares_process_fd(channel_.get(), read_fd, write_fd);
            co_return;
        }

        ares_process_fd(channel_.get(), ARES_SOCKET_BAD, ARES_SOCKET_BAD);
    }

    static std::chrono::nanoseconds as_duration(timeval value)
    {
        return std::chrono::seconds{value.tv_sec}
            + std::chrono::microseconds{value.tv_usec};
    }

    std::unique_ptr<ares_channel_t, channel_deleter> channel_;
};

} // namespace nxt::rt
