#include <nxt/rt/cares.hpp>
#include <nxt/rt/uring_wand.hpp>

#include "test.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

template<typename T>
T pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<T> & task)
{
    wand.run_until_done(deck, task);
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
