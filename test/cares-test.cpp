#include <nxtrt/cares.hpp>
#include <nxtrt/wand/uring.hpp>

#include "test.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

template<typename T>
T pump_until_done(
    nxtrt::deck & deck,
    nxtrt::uring_wand & wand,
    nxtrt::task<T> & task)
{
    wand.run_until_done(deck, task);
    return std::move(task).result();
}

nxtrt::task<std::vector<nxtrt::resolved_address>> resolve_localhost()
{
    auto resolver = nxtrt::cares_resolver{};
    co_return co_await resolver.getaddrinfo("localhost", "80");
}

static suite cares_tests{
    "c-ares", [] {
        "resolver"_test = [] {
            "localhost resolves to IPv4 loopback"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
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
