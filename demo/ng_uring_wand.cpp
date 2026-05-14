#include <nxt/rt/uring_wand.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

nxt::rt::task<std::string> read_file_with_wand(std::string path)
{
    auto fd = co_await nxt::rt::openat_wish{
        .dirfd = AT_FDCWD,
        .path = std::move(path),
        .flags = O_RDONLY,
        .mode = 0,
    };

    auto bytes = std::array<std::byte, 4096>{};
    auto n = co_await nxt::rt::read_some_wish{
        .fd = fd,
        .buffer = std::span<std::byte>{bytes},
        .offset = 0,
    };
    ::close(fd);

    co_return std::string{
        reinterpret_cast<char const *>(bytes.data()),
        reinterpret_cast<char const *>(bytes.data()) + n};
}

void pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<std::string> & task)
{
    for (auto spins = 0; spins != 1000 && !task.done(); ++spins) {
        if (!deck.empty())
            deck.run_ready(wand);
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

    auto deck = nxt::rt::deck{};
    auto wand = nxt::rt::uring_wand{};
    auto task = read_file_with_wand(path);

    deck.start(task);
    pump_until_done(deck, wand, task);

    std::cout << std::move(task).result();
    return 0;
} catch (std::exception const & error) {
    std::cerr << "ng-uring-wand-demo: " << error.what() << '\n';
    return 1;
}
