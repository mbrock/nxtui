#include <nxtrt/net_dns.hpp>
#include <nxtrt/arch.hpp>

#include "test.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

template<typename T>
T cares_pump_until_done(
    nxtrt::deck & deck,
    nxtrt::arch::wand & wand,
    nxtrt::task<T> & task)
{
    wand.run_until_done(deck, task);
    return std::move(task).result();
}

nxtrt::task<std::vector<nxtrt::resolved_address>> resolve_localhost()
{
    co_return co_await nxtrt::net::resolve_tcp("localhost", "80");
}

static suite dns_tests{
    "DNS", [] {
        "resolver"_group = [] {
            "localhost resolves to IPv4 loopback"_test = [] {
                auto wand = nxtrt::arch::wand{};
                auto deck = nxtrt::deck{&wand};
                auto root = nxtrt::root_task{
                    deck,
                    [] {
                        return resolve_localhost();
                    },
                };

                root.start();
                auto addresses = cares_pump_until_done(
                    deck,
                    wand,
                    root.inner());

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
#if defined(NXTRT_HAVE_CARES)
            "cancelled high-fd lookups drain callbacks before frame destruction"_test =
                [] {
                    struct fd_limit_guard
                    {
                        rlimit previous{};
                        ~fd_limit_guard()
                        {
                            (void) ::setrlimit(RLIMIT_NOFILE, &previous);
                        }
                    };
                    auto previous = rlimit{};
                    if (::getrlimit(RLIMIT_NOFILE, &previous) != 0)
                        throw std::runtime_error{"cannot read fd limit"};
                    auto limit = fd_limit_guard{previous};
                    if (limit.previous.rlim_cur < 2048) {
                        auto raised = limit.previous;
                        raised.rlim_cur = 2048;
                        if (::setrlimit(RLIMIT_NOFILE, &raised) != 0)
                            throw std::runtime_error{
                                "cannot raise fd limit"};
                    }
                    // Occupy low descriptors so resolver sockets exceed
                    // fd_set's conventional 1024-descriptor limit. No
                    // responses are needed: cancel while the lookup is
                    // parked, before processing DNS IO.
                    auto occupied = std::vector<nxt::unique_fd>{};
                    while (occupied.empty()
                           || occupied.back().get() < 1024) {
                        auto fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                        if (fd < 0)
                            throw std::runtime_error{
                                "cannot reserve DNS descriptors"};
                        occupied.emplace_back(fd);
                    }
                    auto resolver = nxtrt::cares_resolver{};
                    auto wand = nxtrt::arch::wand{};
                    auto deck = nxtrt::deck{&wand};
                    {
                        auto root = nxtrt::root_task{
                            deck, [&] {
                                return resolver.getaddrinfo(
                                    "cancel-lookup.example", "80");
                            }};
                        root.start();
                        // Stage the socket wishes as well as the lookup,
                        // without consuming any platform completions.
                        deck.run_until_idle();
                        expect(!root.inner().done());
                        root.inner().request_stop();
                        wand.run_until_done(deck, root.inner());
                        auto cancelled = false;
                        try {
                            (void) std::move(root.inner()).result();
                        } catch (const nxtrt::operation_cancelled &) {
                            cancelled = true;
                        }
                        expect(cancelled);
                    }
                    auto next = nxtrt::root_task{
                        deck, [&] {
                            return resolver.getaddrinfo("127.0.0.1", "80");
                        }};
                    next.start();
                    auto addresses =
                        cares_pump_until_done(deck, wand, next.inner());
                    expect(!addresses.empty());
                    expect(addresses.at(0).family == AF_INET);
                };
#endif
        };
    }};

} // namespace nxt::test
