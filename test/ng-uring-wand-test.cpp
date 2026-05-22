#include <nxt/rt/buffers.hpp>
#include <nxt/rt/uring_wand.hpp>

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
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace nxt::test {

using namespace boost::ut;

class unique_fd
{
public:
    explicit unique_fd(int fd = -1) noexcept
        : fd_(fd)
    {}

    unique_fd(const unique_fd &) = delete;
    unique_fd & operator=(const unique_fd &) = delete;

    unique_fd(unique_fd && other) noexcept
        : fd_(std::exchange(other.fd_, -1))
    {}

    unique_fd & operator=(unique_fd && other) noexcept
    {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~unique_fd()
    {
        close();
    }

    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }

private:
    void close() noexcept
    {
        if (fd_ >= 0)
            ::close(std::exchange(fd_, -1));
    }

    int fd_ = -1;
};

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

    auto revents = co_await nxt::rt::poll_wish{
        .fd = rx,
        .events = POLLIN,
    };
    if ((revents & POLLIN) == 0)
        throw std::runtime_error{"poll did not report readable socket"};
}

nxt::rt::task<void> timeout_once()
{
    co_await nxt::rt::timeout_wish::after(1ms);
}

nxt::rt::task<void> poll_until_after_socket_send(int tx, int rx)
{
    auto message = std::string_view{"y"};
    auto sent = co_await nxt::rt::send_some(tx, nxt::rt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short poll-until smoke send"};

    auto result = co_await nxt::rt::poll_until_wish::after(rx, POLLIN, 1s);
    if (result.timed_out || (result.events & POLLIN) == 0)
        throw std::runtime_error{"poll-until did not report readable socket"};
}

nxt::rt::task<void> poll_until_timeout(int rx)
{
    auto result = co_await nxt::rt::poll_until_wish::after(rx, POLLIN, 1ms);
    if (!result.timed_out)
        throw std::runtime_error{"poll-until did not time out"};
}

nxt::rt::task<void> poll_forever(int rx)
{
    (void)co_await nxt::rt::poll_wish{
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
        (void)co_await nxt::rt::poll_wish{
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
    co_await nxt::rt::connect_wish::from(
        fd,
        reinterpret_cast<sockaddr const *>(&address),
        sizeof(address));
}

nxt::rt::task<struct statx> stat_current_directory()
{
    co_return co_await nxt::rt::statx_wish{
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
    auto fd = co_await nxt::rt::openat_wish{
        .path = ".",
        .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
    };
    auto dir = unique_fd{fd};

    auto storage = std::array<std::byte, 4096>{};
    auto bytes = co_await nxt::rt::getdents64_wish{
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

template<typename T>
T pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<T> & task)
{
    for (auto spins = 0; spins != 1000 && !task.done(); ++spins) {
        if (!deck.empty())
            deck.run_ready();
        wand.poll(deck);
        if (deck.empty() && !task.done())
            std::this_thread::sleep_for(1ms);
    }

    if (!task.done())
        throw std::runtime_error{"uring socket smoke test did not complete"};

    return std::move(task).result();
}

void pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<void> & task)
{
    for (auto spins = 0; spins != 1000 && !task.done(); ++spins) {
        if (!deck.empty())
            deck.run_ready();
        wand.poll(deck);
        if (deck.empty() && !task.done())
            std::this_thread::sleep_for(1ms);
    }

    if (!task.done())
        throw std::runtime_error{"uring socket smoke test did not complete"};

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
        "socket I/O"_test = [] {
            "echoes over a socketpair"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = unique_fd{sockets[0]};
                auto second = unique_fd{sockets[1]};

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

                auto first = unique_fd{sockets[0]};
                auto second = unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = poll_after_socket_send(first.get(), second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "loopback listeners accept connected clients"_test = [] {
                auto listener = unique_fd{::socket(AF_INET, SOCK_STREAM, 0)};
                if (listener.get() < 0)
                    throw std::runtime_error{"listener socket failed"};
                auto address = loopback_listener_address(listener.get());

                auto client = unique_fd{::socket(AF_INET, SOCK_STREAM, 0)};
                if (client.get() < 0)
                    throw std::runtime_error{"client socket failed"};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = connect_to(client.get(), address);

                deck.start(task);
                pump_until_done(deck, wand, task);

                auto accepted =
                    unique_fd{::accept(listener.get(), nullptr, nullptr)};
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

                auto first = unique_fd{sockets[0]};
                auto second = unique_fd{sockets[1]};

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

                auto first = unique_fd{sockets[0]};
                auto second = unique_fd{sockets[1]};

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

                auto first = unique_fd{sockets[0]};
                auto second = unique_fd{sockets[1]};

                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = poll_until_stopped(second.get());

                deck.start(task);
                deck.run_ready();
                expect(!task.done());

                task.request_stop();
                for (auto spins = 0; spins != 1000 && !task.done(); ++spins) {
                    if (!deck.empty())
                        deck.run_ready();
                    wand.wave(deck);
                    wand.poll(deck);
                    if (deck.empty() && !task.done())
                        std::this_thread::sleep_for(1ms);
                }

                if (!task.done())
                    throw std::runtime_error{"poll cancellation did not complete"};
                std::move(task).result();
            };

            "with_timeout returns when the body wins"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = unique_fd{sockets[0]};
                auto second = unique_fd{sockets[1]};

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

                auto first = unique_fd{sockets[0]};
                auto second = unique_fd{sockets[1]};

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
