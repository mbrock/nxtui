#include <nxt/rt/buffers.hpp>
#include <nxt/rt/fs.hpp>
#include <nxt/rt/uring_wand.hpp>
#include <nxt/unique-fd.hpp>

#include "test.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace nxt::test {

using namespace boost::ut;

nxt::rt::task<std::string> echo_over_socketpair(int tx, int rx)
{
    auto message = std::string_view{"socket wish smoke"};
    auto sent = co_await nxt::rt::send_some(tx, nxt::rt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short socket send"};

    auto storage = std::array<std::byte, 64>{};
    auto source = nxt::rt::socket_source{rx};
    auto reader = nxt::rt::byte_reader{source, std::span{storage}};
    auto chunk = co_await reader.take_some();
    if (!chunk)
        throw std::runtime_error{"socket recv reached eof"};

    co_return std::string{nxt::rt::as_string_view(*chunk)};
}

nxt::rt::task<void> poll_after_socket_send(int tx, int rx)
{
    auto message = std::string_view{"x"};
    auto sent = co_await nxt::rt::send_some(tx, nxt::rt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short poll smoke send"};

    auto revents = co_await nxt::rt::op::poll{
        .fd = rx,
        .events = POLLIN,
    };
    if ((revents & POLLIN) == 0)
        throw std::runtime_error{"poll did not report readable socket"};
}

nxt::rt::task<void> timeout_once()
{
    co_await nxt::rt::op::timeout::after(1ms);
}

nxt::rt::task<void> poll_until_after_socket_send(int tx, int rx)
{
    auto message = std::string_view{"y"};
    auto sent = co_await nxt::rt::send_some(tx, nxt::rt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short poll-until smoke send"};

    auto result = co_await nxt::rt::op::poll_until::after(rx, POLLIN, 1s);
    if (result.timed_out || (result.events & POLLIN) == 0)
        throw std::runtime_error{"poll-until did not report readable socket"};
}

nxt::rt::task<void> poll_until_timeout(int rx)
{
    auto result = co_await nxt::rt::op::poll_until::after(rx, POLLIN, 1ms);
    if (!result.timed_out)
        throw std::runtime_error{"poll-until did not time out"};
}

nxt::rt::task<void> poll_forever(int rx)
{
    (void)co_await nxt::rt::op::poll{
        .fd = rx,
        .events = POLLIN,
    };
}

nxt::rt::task<void> poll_with_timeout(int rx)
{
    co_await nxt::rt::with_timeout(1ms, poll_forever(rx));
}

nxt::rt::task<void> poll_after_send_with_timeout(int tx, int rx)
{
    co_await nxt::rt::with_timeout(1s, poll_after_socket_send(tx, rx));
}

nxt::rt::task<void> poll_until_stopped(int rx)
{
    try {
        (void)co_await nxt::rt::op::poll{
            .fd = rx,
            .events = POLLIN,
        };
    } catch (const nxt::rt::operation_cancelled &) {
        co_return;
    }

    throw std::runtime_error{"poll completed instead of being cancelled"};
}

nxt::rt::task<void> connect_to(int fd, sockaddr_in address)
{
    co_await nxt::rt::op::connect::from(
        fd,
        reinterpret_cast<sockaddr const *>(&address),
        sizeof(address));
}

nxt::rt::task<struct statx> stat_current_directory()
{
    co_return co_await nxt::rt::op::statx{
        .path = ".",
        .mask = STATX_TYPE,
    };
}

struct linux_dirent64
{
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

nxt::rt::task<std::vector<std::string>> read_current_directory_names()
{
    auto fd = co_await nxt::rt::op::openat{
        .path = ".",
        .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
    };
    auto dir = nxt::unique_fd{fd};

    auto storage = std::array<std::byte, 4096>{};
    auto bytes = co_await nxt::rt::op::getdents64{
        .fd = dir.get(),
        .buffer = storage,
    };

    auto names = std::vector<std::string>{};
    for (auto offset = std::size_t{}; offset < bytes;) {
        auto const * entry = reinterpret_cast<linux_dirent64 const *>(
            storage.data() + offset);
        if (entry->d_reclen == 0)
            throw std::runtime_error{"getdents64 returned a zero-length entry"};

        names.emplace_back(entry->d_name);
        offset += entry->d_reclen;
    }
    co_return names;
}

nxt::rt::task<void> write_to_fd(int fd, std::string_view text)
{
    auto written = co_await nxt::rt::op::write_some{
        .fd = fd,
        .buffer = nxt::rt::as_bytes(text),
    };
    if (written != text.size())
        throw std::runtime_error{"short write wish"};
}

template<typename T>
T pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<T> & task)
{
    wand.run_until_done(deck, task);
    return std::move(task).result();
}

void pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<void> & task)
{
    wand.run_until_done(deck, task);
    std::move(task).result();
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

static suite ng_uring_wand_tests{
    "uring wand", [] {
        "runner"_test = [] {
            "runs task factories"_test = [] {
                auto value = nxt::rt::run([]() -> nxt::rt::task<int> {
                    co_await nxt::rt::op::manual{};
                    co_return 42;
                });

                expect(value == 42_i);
            };
        };

        "socket I/O"_test = [] {
            "echoes over a socketpair"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = echo_over_socketpair(first.get(), second.get());

                deck.start(task);
                auto echoed = pump_until_done(deck, wand, task);

                expect(echoed == "socket wish smoke");
            };

            "socket sends complete before readability is polled"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = poll_after_socket_send(first.get(), second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

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

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = connect_to(client.get(), address);

                deck.start(task);
                pump_until_done(deck, wand, task);

                auto accepted =
                    nxt::unique_fd{::accept(listener.get(), nullptr, nullptr)};
                expect(accepted.get() >= 0);
            };
        };

        "file I/O"_test = [] {
            "statx wishes return file metadata"_test = [] {
                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = stat_current_directory();

                deck.start(task);
                auto stat = pump_until_done(deck, wand, task);

                expect((stat.stx_mask & STATX_TYPE) != 0);
                expect(S_ISDIR(stat.stx_mode));
            };

            "getdents64 wishes return directory entries"_test = [] {
                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = read_current_directory_names();

                deck.start(task);
                auto names = pump_until_done(deck, wand, task);

                expect(std::ranges::find(names, ".") != names.end());
                expect(std::ranges::find(names, "..") != names.end());
            };

            "fs lists portable directory entries"_test = [] {
                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = nxt::rt::fs::list_path(".");

                deck.start(task);
                auto entries = pump_until_done(deck, wand, task);

                auto dot = std::ranges::find(
                    entries,
                    ".",
                    &nxt::rt::fs::directory_entry::name);
                expect(dot != entries.end());
                expect(dot->status.kind == nxt::rt::fs::file_kind::directory);
            };
        };

        "file descriptor I/O"_test = [] {
            "write wishes write to file descriptors"_test = [] {
                auto fds = std::array<int, 2>{-1, -1};
                if (::pipe(fds.data()) != 0)
                    throw std::runtime_error{"pipe failed"};

                auto rx = nxt::unique_fd{fds[0]};
                auto tx = nxt::unique_fd{fds[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = write_to_fd(tx.get(), "wishful stdout");

                deck.start(task);
                pump_until_done(deck, wand, task);

                auto buffer = std::array<char, 32>{};
                auto n = ::read(rx.get(), buffer.data(), buffer.size());
                if (n < 0)
                    throw std::runtime_error{"pipe read failed"};

                expect(std::string_view{buffer.data(), static_cast<std::size_t>(n)}
                    == "wishful stdout");
            };
        };

        "timers and polling"_test = [] {
            "timeout wishes complete"_test = [] {
                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = timeout_once();

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "readiness is reported before a poll-until deadline"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task =
                    poll_until_after_socket_send(first.get(), second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "quiet watched fds time out"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = poll_until_timeout(second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "poll wishes are cancelled when their task stops"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = poll_until_stopped(second.get());

                deck.start(task);
                deck.run_ready();
                expect(!task.done());

                task.request_stop();
                wand.run_until_done(deck, task);
                std::move(task).result();
            };

            "with_timeout returns when the body wins"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task =
                    poll_after_send_with_timeout(first.get(), second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "with_timeout throws when the timer wins"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = poll_with_timeout(second.get());

                deck.start(task);

                auto timed_out = false;
                try {
                    pump_until_done(deck, wand, task);
                } catch (const nxt::rt::timeout_error &) {
                    timed_out = true;
                }

                expect(timed_out);
            };
        };
    }};

} // namespace nxt::test
