#include <nxtrt/buffers.hpp>
#include <nxtrt/wand/epoll.hpp>
#include <nxt/unique-fd.hpp>

#include "test.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>

using namespace std::chrono_literals;

namespace nxt::test {

using namespace boost::ut;

#if NXT_RT_HAS_EPOLL

void epoll_pump_until_done(
    nxtrt::deck & deck, nxtrt::epoll_wand & wand, nxtrt::task<void> & task)
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

template<class T>
nxtrt::task<T> timeout_body_failure()
{
    co_await nxtrt::yield();
    throw std::domain_error{"original timeout body failure"};
}

nxtrt::task<void> timeout_body_wait()
{
    co_await nxtrt::op::timeout::after(10s);
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
        (void) co_await nxtrt::op::poll{rx, POLLIN};
    } catch (const nxtrt::operation_cancelled &) {
        co_return;
    }

    throw std::runtime_error{
        "epoll poll completed instead of being cancelled"};
}

nxtrt::task<int> epoll_poll_fd(int fd)
{
    co_return co_await nxtrt::op::poll{fd, POLLIN};
}

nxtrt::task<std::size_t> epoll_read_one(int fd, std::span<std::byte> buffer)
{
    co_return co_await nxtrt::op::read_some{fd, buffer};
}

static suite epoll_wand_tests{
    "epoll wand", [] {
        "timeout wishes complete"_test = [] {
            auto wand = nxtrt::epoll_wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck,
                [] { return epoll_timeout_once(); },
            };

            root.start();
#ifdef __FILC__
            // The hub must keep parked execs alive independently of the weak
            // pointer table used for integer tokens and kernel event data.
            deck.run_ready();
            wand.wave(deck);
            zgc_request_and_wait();
#endif
            epoll_pump_until_done(deck, wand, root.inner());

            expect(root.inner().done());
        };

        "with_timeout preserves the body's original exception"_test = [] {
            auto check = []<class T> {
                auto wand = nxtrt::epoll_wand{};
                auto deck = nxtrt::deck{&wand};
                auto root = nxtrt::root_task{
                    deck, [] {
                        return nxtrt::with_timeout(
                            10s, timeout_body_failure<T>());
                    }};
                root.start();
                wand.run_until_done(deck, root.inner());
                auto original = false;
                try {
                    std::move(root.inner()).result();
                } catch (const std::domain_error & error) {
                    original = std::string_view{error.what()}
                               == "original timeout body failure";
                }
                expect(original);
            };
            check.operator()<void>();
            check.operator()<int>();
        };

        "with_timeout accepts stop at every startup turn"_test = [] {
            for (int turns = 0; turns < 20; ++turns) {
                auto wand = nxtrt::epoll_wand{};
                auto deck = nxtrt::deck{&wand};
                auto root =
                    nxtrt::root_task{deck, [] {
                                         return nxtrt::with_timeout(
                                             10s, timeout_body_wait());
                                     }};
                root.start();
                for (int i = 0; i < turns; ++i)
                    deck.run_ready();
                root.inner().request_stop();
                wand.run_until_done(deck, root.inner());
                auto cancelled = false;
                try {
                    std::move(root.inner()).result();
                } catch (const nxtrt::operation_cancelled &) {
                    cancelled = true;
                }
                expect(cancelled);
            }
        };

        "native poll-until reports readiness"_test = [] {
            auto sockets = make_epoll_socketpair();
            auto original_flags = ::fcntl(sockets[0].get(), F_GETFL, 0);
            auto wand = nxtrt::epoll_wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck,
                [&] {
                    return epoll_poll_until_after_socket_send(
                        sockets[0].get(), sockets[1].get());
                },
            };

            root.start();
            epoll_pump_until_done(deck, wand, root.inner());

            expect(root.inner().done());
            expect(::fcntl(sockets[0].get(), F_GETFL, 0) == original_flags);
        };

        "read retries EAGAIN without changing blocking socket flags"_test =
            [] {
                auto sockets = make_epoll_socketpair();
                auto original_flags = ::fcntl(sockets[1].get(), F_GETFL, 0);
                auto buffer = std::array<std::byte, 1>{};
                auto wand = nxtrt::epoll_wand{};
                auto deck = nxtrt::deck{&wand};
                auto root = nxtrt::root_task{
                    deck,
                    [&] {
                        return epoll_read_one(sockets[1].get(), buffer);
                    },
                };

                root.start();
                deck.run_ready();
                wand.wave(
                    deck); // The first read must return EAGAIN, not block.
                expect(
                    ::fcntl(sockets[1].get(), F_GETFL, 0)
                    == original_flags);
                auto byte = std::byte{'x'};
                expect(::write(sockets[0].get(), &byte, 1) == 1);
                wand.run_until_done(deck, root.inner());

                expect(std::move(root.inner()).result() == 1_ul);
                expect(buffer[0] == byte);
                expect(
                    ::fcntl(sockets[1].get(), F_GETFL, 0)
                    == original_flags);
            };

        "stale readiness rearms instead of failing the losing read"_test =
            [] {
                auto sockets = make_epoll_socketpair();
                auto alias = nxt::unique_fd{
                    ::fcntl(sockets[1].get(), F_DUPFD_CLOEXEC, 0)};
                if (alias.get() < 0)
                    throw std::runtime_error{"dup failed"};
                auto first_byte = std::array<std::byte, 1>{};
                auto second_byte = std::array<std::byte, 1>{};
                auto wand = nxtrt::epoll_wand{};
                auto deck = nxtrt::deck{&wand};
                auto first = nxtrt::root_task{
                    deck, [&] {
                        return epoll_read_one(sockets[1].get(), first_byte);
                    }};
                auto second = nxtrt::root_task{
                    deck, [&] {
                        return epoll_read_one(alias.get(), second_byte);
                    }};
                first.start();
                second.start();
                deck.run_ready();
                expect(::write(sockets[0].get(), "x", 1) == 1);
                // Both registrations are ready, but only the first callback
                // can consume x. The second callback must see EAGAIN and
                // rearm.
                wand.poll(deck);
                expect(
                    (first_byte[0] == std::byte{'x'})
                    != (second_byte[0] == std::byte{'x'}));
                expect(::write(sockets[0].get(), "y", 1) == 1);
                wand.run_until_done(deck, first.inner());
                wand.run_until_done(deck, second.inner());
                expect(std::move(first.inner()).result() == 1_ul);
                expect(std::move(second.inner()).result() == 1_ul);
                expect(
                    (first_byte[0] == std::byte{'x'}
                     && second_byte[0] == std::byte{'y'})
                    || (first_byte[0] == std::byte{'y'}
                        && second_byte[0] == std::byte{'x'}));
            };

        "poll registration failure fails only its wish"_test = [] {
            auto fd = nxt::unique_fd{::open("/dev/null", O_RDONLY)};
            if (fd.get() < 0)
                throw std::runtime_error{"open /dev/null failed"};

            auto wand = nxtrt::epoll_wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck,
                [&] { return epoll_poll_fd(fd.get()); },
            };

            root.start();
            auto failed = false;
            try {
                wand.run_until_done(deck, root.inner());
                (void) std::move(root.inner()).result();
            } catch (const nxtrt::errno_error &) {
                failed = true;
            }
            expect(failed);
        };

        "native poll-until times out"_test = [] {
            auto sockets = make_epoll_socketpair();
            auto wand = nxtrt::epoll_wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck,
                [&] { return epoll_poll_until_timeout(sockets[1].get()); },
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
                [&] { return epoll_poll_cancelled(sockets[1].get()); },
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
