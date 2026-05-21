#include <nxt/rt/cares.hpp>
#include <nxt/rt/uring_wand.hpp>

#include "test.hpp"

#include <chrono>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace nxt::test {

using namespace boost::ut;

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

static suite ng_cares_tests{
    "c-ares", [] {
        "resolver"_test = [] {
            "localhost resolves to IPv4 loopback"_test = [] {
                auto wand = nxt::rt::uring_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = resolve_localhost();

                deck.start(task);
                auto addresses = pump_until_done(deck, wand, task);

                expect(!addresses.empty())
                    << "localhost should resolve to at least one address";

                auto has_loopback = false;
                for (auto const & address : addresses) {
                    if (address.family == AF_INET) {
                        auto const * in = reinterpret_cast<
                            sockaddr_in const *>(address.sockaddr_ptr());
                        has_loopback =
                            has_loopback
                            || ntohl(in->sin_addr.s_addr) == INADDR_LOOPBACK;
                    }
                }

                expect(has_loopback)
                    << "localhost should include IPv4 loopback";
            };
        };
    }};

} // namespace nxt::test
