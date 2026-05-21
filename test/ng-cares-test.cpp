#include <nxt/rt/cares.hpp>
#include <nxt/rt/uring_wand.hpp>

#include <chrono>
#include <netinet/in.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;

namespace {

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
        throw std::runtime_error{"c-ares resolver smoke test did not complete"};

    return std::move(task).result();
}

nxt::rt::task<std::vector<nxt::rt::resolved_address>> resolve_localhost()
{
    auto resolver = nxt::rt::cares_resolver{};
    co_return co_await resolver.getaddrinfo("localhost", "80");
}

} // namespace

int main()
try {
    auto wand = nxt::rt::uring_wand{};
    auto deck = nxt::rt::deck{&wand};
    auto task = resolve_localhost();

    deck.start(task);
    auto addresses = pump_until_done(deck, wand, task);
    if (addresses.empty())
        throw std::runtime_error{"localhost resolved to no addresses"};

    auto has_loopback = false;
    for (auto const & address : addresses) {
        if (address.family == AF_INET) {
            auto const * in =
                reinterpret_cast<sockaddr_in const *>(address.sockaddr_ptr());
            has_loopback =
                has_loopback || ntohl(in->sin_addr.s_addr) == INADDR_LOOPBACK;
        }
    }
    if (!has_loopback)
        throw std::runtime_error{"localhost did not resolve to IPv4 loopback"};

    return 0;
} catch (std::exception const & error) {
    write(STDERR_FILENO, error.what(), std::string_view{error.what()}.size());
    write(STDERR_FILENO, "\n", 1);
    return 1;
}
