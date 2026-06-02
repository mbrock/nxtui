#include <nxtrt/buffers.hpp>
#include <nxtrt/wand/kqueue.hpp>
#include <nxt/unique-fd.hpp>

#include "test.hpp"

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <utility>

using namespace std::chrono_literals;

namespace nxt::test {

using namespace boost::ut;

#if NXT_RT_HAS_KQUEUE

template<typename T>
T kqueue_pump_until_done(
    nxtrt::deck & deck,
    nxtrt::kqueue_wand & wand,
    nxtrt::task<T> & task)
{
    wand.run_until_done(deck, task);
    return std::move(task).result();
}

void kqueue_pump_until_done(
    nxtrt::deck & deck,
    nxtrt::kqueue_wand & wand,
    nxtrt::task<void> & task)
{
    wand.run_until_done(deck, task);
    std::move(task).result();
}

std::array<nxt::unique_fd, 2> make_socketpair()
{
    auto sockets = std::array<int, 2>{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
        throw std::runtime_error{"socketpair failed"};

    return {
        nxt::unique_fd{sockets[0]},
        nxt::unique_fd{sockets[1]},
    };
}

nxtrt::task<void> timeout_once()
{
    co_await nxtrt::op::timeout::after(1ms);
}

nxtrt::task<void> native_poll_until_after_socket_send(int tx, int rx)
{
    auto message = std::string_view{"x"};
    auto sent = co_await nxtrt::send_some(tx, nxtrt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short poll-until send"};

    auto result = co_await nxtrt::op::poll_until::after(rx, POLLIN, 1s);
    if (result.timed_out || (result.events & POLLIN) == 0)
        throw std::runtime_error{"native poll-until missed readability"};
}

nxtrt::task<void> native_poll_until_timeout(int rx)
{
    auto result = co_await nxtrt::op::poll_until::after(rx, POLLIN, 1ms);
    if (!result.timed_out)
        throw std::runtime_error{"native poll-until did not time out"};
}

nxtrt::task<nxt::unique_fd> accept_one(int listener)
{
    co_return nxt::unique_fd{co_await nxtrt::op::accept{.fd = listener}};
}

sockaddr_in loopback_listener_address(int fd)
{
    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (::bind(
            fd,
            reinterpret_cast<sockaddr const *>(&address),
            sizeof(address)) != 0)
        throw std::runtime_error{"bind failed"};
    if (::listen(fd, 1) != 0)
        throw std::runtime_error{"listen failed"};

    auto size = socklen_t{sizeof(address)};
    if (::getsockname(
            fd,
            reinterpret_cast<sockaddr *>(&address),
            &size) != 0)
        throw std::runtime_error{"getsockname failed"};

    return address;
}

nxtrt::task<void> repeat_native_poll_until(int tx, int rx)
{
    auto message = std::string_view{"z"};
    auto storage = std::array<std::byte, 1>{};

    for (auto i = 0; i != 16; ++i) {
        auto sent = co_await nxtrt::send_some(tx, nxtrt::as_bytes(message));
        if (sent != message.size())
            throw std::runtime_error{"short repeat send"};

        auto result = co_await nxtrt::op::poll_until::after(rx, POLLIN, 1s);
        if (result.timed_out || (result.events & POLLIN) == 0)
            throw std::runtime_error{"repeat poll-until missed readability"};

        auto received = co_await nxtrt::op::recv_some{
            .fd = rx,
            .buffer = storage,
        };
        if (received != storage.size())
            throw std::runtime_error{"short repeat recv"};

        co_await nxtrt::op::timeout::after(1ms);
    }
}

nxtrt::task<void> repeat_poll_with_sibling_filter(int tx, int rx)
{
    auto message = std::string_view{"p"};
    auto storage = std::array<std::byte, 1>{};

    for (auto i = 0; i != 16; ++i) {
        auto events = co_await nxtrt::op::poll{
            .fd = tx,
            .events = POLLIN | POLLOUT,
        };
        if ((events & POLLOUT) == 0)
            throw std::runtime_error{"poll did not report writability"};

        auto sent = co_await nxtrt::send_some(rx, nxtrt::as_bytes(message));
        if (sent != message.size())
            throw std::runtime_error{"short sibling send"};

        auto received = co_await nxtrt::op::recv_some{
            .fd = tx,
            .buffer = storage,
        };
        if (received != storage.size())
            throw std::runtime_error{"short sibling recv"};

        co_await nxtrt::op::timeout::after(1ms);
    }
}

nxtrt::task<void> poll_cancelled(int rx)
{
    try {
        (void)co_await nxtrt::op::poll{
            .fd = rx,
            .events = POLLIN,
        };
    } catch (const nxtrt::operation_cancelled &) {
        co_return;
    }

    throw std::runtime_error{"poll completed instead of being cancelled"};
}

nxtrt::task<void> native_poll_until_cancelled(int rx)
{
    try {
        (void)co_await nxtrt::op::poll_until::after(rx, POLLIN, 1s);
    } catch (const nxtrt::operation_cancelled &) {
        co_return;
    }

    throw std::runtime_error{
        "native poll-until completed instead of being cancelled"};
}

static suite kqueue_wand_tests{
    "kqueue wand", [] {
        "timeout wishes complete"_test = [] {
            auto wand = nxtrt::kqueue_wand{};
            auto deck = nxtrt::deck{&wand};
            auto task = timeout_once();

            deck.start(task);
            kqueue_pump_until_done(deck, wand, task);

            expect(task.done());
        };

        "native poll-until reports readiness"_test = [] {
            auto sockets = make_socketpair();
            auto wand = nxtrt::kqueue_wand{};
            auto deck = nxtrt::deck{&wand};
            auto task = native_poll_until_after_socket_send(
                sockets[0].get(),
                sockets[1].get());

            deck.start(task);
            kqueue_pump_until_done(deck, wand, task);

            expect(task.done());
        };

        "native poll-until times out"_test = [] {
            auto sockets = make_socketpair();
            auto wand = nxtrt::kqueue_wand{};
            auto deck = nxtrt::deck{&wand};
            auto task = native_poll_until_timeout(sockets[1].get());

            deck.start(task);
            kqueue_pump_until_done(deck, wand, task);

            expect(task.done());
        };

        "loopback listeners accept connected clients"_test = [] {
            auto listener = nxt::unique_fd{::socket(AF_INET, SOCK_STREAM, 0)};
            if (listener.get() < 0)
                throw std::runtime_error{"listener socket failed"};
            auto address = loopback_listener_address(listener.get());

            auto client = nxt::unique_fd{::socket(AF_INET, SOCK_STREAM, 0)};
            if (client.get() < 0)
                throw std::runtime_error{"client socket failed"};

            auto wand = nxtrt::kqueue_wand{};
            auto deck = nxtrt::deck{&wand};
            auto task = accept_one(listener.get());

            deck.start(task);
            if (::connect(
                    client.get(),
                    reinterpret_cast<sockaddr const *>(&address),
                    sizeof(address)) != 0)
                throw std::runtime_error{"client connect failed"};

            auto accepted = kqueue_pump_until_done(deck, wand, task);
            expect(accepted.get() >= 0);
        };

        "native poll-until slots are reusable after sibling deletes"_test = [] {
            auto sockets = make_socketpair();
            auto wand = nxtrt::kqueue_wand{};
            auto deck = nxtrt::deck{&wand};
            auto task = repeat_native_poll_until(
                sockets[0].get(),
                sockets[1].get());

            deck.start(task);
            kqueue_pump_until_done(deck, wand, task);

            expect(task.done());
        };

        "poll slots are reusable after sibling deletes"_test = [] {
            auto sockets = make_socketpair();
            auto wand = nxtrt::kqueue_wand{};
            auto deck = nxtrt::deck{&wand};
            auto task = repeat_poll_with_sibling_filter(
                sockets[0].get(),
                sockets[1].get());

            deck.start(task);
            kqueue_pump_until_done(deck, wand, task);

            expect(task.done());
        };

        "poll wishes are cancelled when their task stops"_test = [] {
            auto sockets = make_socketpair();
            auto wand = nxtrt::kqueue_wand{};
            auto deck = nxtrt::deck{&wand};
            auto task = poll_cancelled(sockets[1].get());

            deck.start(task);
            deck.run_ready();
            expect(!task.done());

            task.request_stop();
            wand.run_until_done(deck, task);
            std::move(task).result();
        };

        "native poll-until is cancelled when its task stops"_test = [] {
            auto sockets = make_socketpair();
            auto wand = nxtrt::kqueue_wand{};
            auto deck = nxtrt::deck{&wand};
            auto task = native_poll_until_cancelled(sockets[1].get());

            deck.start(task);
            deck.run_ready();
            expect(!task.done());

            task.request_stop();
            wand.run_until_done(deck, task);
            std::move(task).result();
        };
    }};

#endif

} // namespace nxt::test
