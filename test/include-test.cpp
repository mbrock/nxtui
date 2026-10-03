#include <nxt/nxt.hpp>
#include <nxtrt.hpp>

#include "test.hpp"

#include <concepts>

#if defined(NXTRT_DEFAULT_WAND)
// Each CI matrix leg must actually use its requested application backend.
static_assert(std::same_as<nxtrt::arch::wand, nxtrt::NXTRT_DEFAULT_WAND>);
#endif

#ifdef __FILC__
static_assert(!nxtrt::has_uring_wand);
static_assert(std::same_as<nxtrt::arch::wand, nxtrt::epoll_wand>);
#endif

namespace nxt::test {

using namespace boost::ut;

static boost::ut::suite public_include_tests{
    "PUBLIC INCLUDES", [] {
    "core umbrella does not pull the legacy app runtime"_test = [] {
        auto event = nxtui::input::KeyEvent{};
        boost::ut::expect(event.key == nxtui::input::Key::unknown);
    };

    "runtime umbrella exposes task"_test = [] {
        auto deck = nxtrt::deck{};
        boost::ut::expect(
            deck.sync_wait([]() -> nxtrt::task<int> { co_return 42; })
            == 42);
    };
}};

} // namespace nxt::test
