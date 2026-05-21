#include <nxt/rt/buffers.hpp>
#include <nxt/rt/uring_wand.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

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

struct file_digest
{
    std::size_t bytes = 0;
    std::uint64_t fnv1a = 14695981039346656037ull;
};

void update_fnv1a(file_digest & digest, std::span<const std::byte> bytes)
{
    for (auto byte : bytes) {
        digest.fnv1a ^= static_cast<std::uint8_t>(byte);
        digest.fnv1a *= 1099511628211ull;
    }
}

nxt::rt::task<file_digest> hash_file_with_wand(std::string path)
{
    auto fd = co_await nxt::rt::openat_wish{
        .dirfd = AT_FDCWD,
        .path = std::move(path),
        .flags = O_RDONLY,
        .mode = 0,
    };
    auto owned_fd = unique_fd{fd};
    auto source = nxt::rt::fd_source{owned_fd.get()};
    auto buffer = std::array<std::byte, 4096>{};
    auto digest = file_digest{};

    digest.bytes = co_await nxt::rt::for_each_chunk(
        source,
        std::span{buffer},
        [&digest](std::span<const std::byte> chunk) {
            update_fnv1a(digest, chunk);
        });
    co_return digest;
}

void pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<file_digest> & task)
{
    for (auto spins = 0; spins != 1000 && !task.done(); ++spins) {
        if (!deck.empty())
            deck.run_ready();
        wand.poll(deck);
        if (deck.empty() && !task.done())
            std::this_thread::sleep_for(1ms);
    }

    if (!task.done())
        throw std::runtime_error{"demo task did not complete"};
}

} // namespace

int main(int argc, char ** argv)
try {
    auto path = argc > 1
        ? std::string{argv[1]}
        : std::string{"/etc/hostname"};

    auto wand = nxt::rt::uring_wand{};
    auto deck = nxt::rt::deck{&wand};
    auto task = hash_file_with_wand(path);

    deck.start(task);
    pump_until_done(deck, wand, task);

    auto digest = std::move(task).result();
    std::cout
        << path << '\n'
        << "bytes " << digest.bytes << '\n'
        << "fnv1a 0x" << std::hex << std::setw(16) << std::setfill('0')
        << digest.fnv1a << '\n';
    return 0;
} catch (std::exception const & error) {
    std::cerr << "ng-uring-wand-demo: " << error.what() << '\n';
    return 1;
}
