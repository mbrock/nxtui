#include <nxtrt/buffers.hpp>
#include <nxtrt/wand/epoll.hpp>
#include <nxt/unique-fd.hpp>

#include "test.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>

using namespace std::chrono_literals;

namespace nxt::test {

using namespace boost::ut;

#if NXT_RT_HAS_EPOLL

void epoll_pump_until_done(
    nxtrt::deck & deck,
    nxtrt::epoll_wand & wand,
    nxtrt::task<void> & task)
{
    wand.run_until_done(deck, task);
    std::move(task).result();
}

std::array<nxt::unique_fd, 2> make_epoll_socketpair()
{
    auto sockets = std::array<int, 2>{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
        throw std::runtime_error{"socketpair failed"};

    return {
        nxt::unique_fd{sockets[0]},
        nxt::unique_fd{sockets[1]},
    };
}

nxtrt::task<void> epoll_timeout_once()
{
    co_await nxtrt::op::timeout::after(1ms);
}

nxtrt::task<void> epoll_poll_until_after_socket_send(int tx, int rx)
{
    auto message = std::string_view{"x"};
    auto sent = co_await nxtrt::send_some(tx, nxtrt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short epoll poll-until send"};

    auto result = co_await nxtrt::op::poll_until::after(rx, POLLIN, 1s);
    if (result.timed_out || (result.events & POLLIN) == 0)
        throw std::runtime_error{"epoll poll-until missed readability"};
}

nxtrt::task<void> epoll_poll_until_timeout(int rx)
{
    auto result = co_await nxtrt::op::poll_until::after(rx, POLLIN, 1ms);
    if (!result.timed_out)
        throw std::runtime_error{"epoll poll-until did not time out"};
}

nxtrt::task<void> epoll_poll_cancelled(int rx)
{
    try {
        (void)co_await nxtrt::op::poll{rx, POLLIN};
    } catch (const nxtrt::operation_cancelled &) {
        co_return;
    }

    throw std::runtime_error{"epoll poll completed instead of being cancelled"};
}

static suite epoll_wand_tests{
    "epoll wand", [] {
        "timeout wishes complete"_test = [] {
            auto wand = nxtrt::epoll_wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck,
                [] {
                    return epoll_timeout_once();
                },
            };

            root.start();
            epoll_pump_until_done(deck, wand, root.inner());

            expect(root.inner().done());
        };

        "native poll-until reports readiness"_test = [] {
            auto sockets = make_epoll_socketpair();
            auto wand = nxtrt::epoll_wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck,
                [&] {
                    return epoll_poll_until_after_socket_send(
                        sockets[0].get(),
                        sockets[1].get());
                },
            };

            root.start();
            epoll_pump_until_done(deck, wand, root.inner());

            expect(root.inner().done());
        };

        "native poll-until times out"_test = [] {
            auto sockets = make_epoll_socketpair();
            auto wand = nxtrt::epoll_wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck,
                [&] {
                    return epoll_poll_until_timeout(sockets[1].get());
                },
            };

            root.start();
            epoll_pump_until_done(deck, wand, root.inner());

            expect(root.inner().done());
        };

        "poll wishes are cancelled when their task stops"_test = [] {
            auto sockets = make_epoll_socketpair();
            auto wand = nxtrt::epoll_wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck,
                [&] {
                    return epoll_poll_cancelled(sockets[1].get());
                },
            };

            root.start();
            deck.run_ready();
            expect(!root.inner().done());

            root.inner().request_stop();
            wand.run_until_done(deck, root.inner());
            std::move(root.inner()).result();
        };
    }};

#endif

} // namespace nxt::test
