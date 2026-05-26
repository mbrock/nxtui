#include <nxt/nxt.hpp>
#include <nxtrt.hpp>

#include "test.hpp"

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
            deck.sync_wait([]() -> nxtrt::task<int> { co_return 42; }())
            == 42);
    };
}};

} // namespace nxt::test
