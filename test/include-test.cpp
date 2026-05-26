#include <nxt/nxt.hpp>
#include <nxt/rt.hpp>

#include "test.hpp"

namespace nxt::test {

using namespace boost::ut;

static boost::ut::suite public_ng_include_tests{
    "PUBLIC NG INCLUDES", [] {
    "core umbrella does not pull the legacy app runtime"_test = [] {
        auto event = nxt::input::KeyEvent{};
        boost::ut::expect(event.key == nxt::input::Key::unknown);
    };

    "runtime umbrella exposes task"_test = [] {
        auto deck = nxt::rt::deck{};
        boost::ut::expect(
            deck.sync_wait([]() -> nxt::rt::task<int> { co_return 42; }())
            == 42);
    };
}};

} // namespace nxt::test
