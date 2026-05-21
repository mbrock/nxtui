#include <nxt/rt/buffers.hpp>
#include <nxt/rt/uring_wand.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;

namespace {

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

template<typename T>
T pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<T> & task)
{
    for (auto spins = 0; spins != 1000 && !task.done(); ++spins) {
        if (!deck.empty())
            deck.run_ready(wand);
        wand.poll(deck);
        if (deck.empty() && !task.done())
            std::this_thread::sleep_for(1ms);
    }

    if (!task.done())
        throw std::runtime_error{"uring socket smoke test did not complete"};

    return std::move(task).result();
}

} // namespace

int main()
try {
    auto sockets = std::array<int, 2>{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
        throw std::runtime_error{"socketpair failed"};

    auto first = unique_fd{sockets[0]};
    auto second = unique_fd{sockets[1]};

    auto deck = nxt::rt::deck{};
    auto wand = nxt::rt::uring_wand{};
    auto task = echo_over_socketpair(first.get(), second.get());

    deck.start(task);
    auto echoed = pump_until_done(deck, wand, task);
    if (echoed != "socket wish smoke")
        throw std::runtime_error{"socket echo mismatch"};

    return 0;
} catch (std::exception const & error) {
    write(STDERR_FILENO, error.what(), std::string_view{error.what()}.size());
    write(STDERR_FILENO, "\n", 1);
    return 1;
}
