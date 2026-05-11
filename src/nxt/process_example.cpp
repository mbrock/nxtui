#include "nxt/any_layout.hpp"
#include "nxt/slot.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"
#include "nxtio/app.hpp"
#include "nxtio/async.hpp"
#include "nxtio/input.hpp"
#include "nxtio/process.hpp"

#include <chrono>
#include <string>

namespace nxt::process_example {

using namespace nxt::tui;
using namespace nxt::ui;
using namespace std::chrono_literals;

nxt::task<> animator(
    Self & self, std::string a, std::string b, Rgba8 color)
{
    int i = 0;
    while (!self.cancelled()) {
        self.draw(text(i % 2 == 0 ? a : b, fg(color)));
        co_await self.sleep(250ms);
        ++i;
    }
}

nxt::task<> root(Self & self)
{
    auto left = self.spawn([](Self & s) {
        return animator(s, "<<<", ">>>", Rgba8::cyan());
    });
    auto right = self.spawn([](Self & s) {
        return animator(s, "***", "...", Rgba8::yellow());
    });

    self.draw(column(
        row(
            left.surface(),
            text(std::string{"  "}),
            right.surface()),
        text(std::string{"press q or esc to quit"},
             fg(Rgba8::white()))));

    using nxt::input::Key;
    while (!self.cancelled()) {
        auto event = co_await self.next_input();
        if (!event)
            co_return;
        if (event->key == Key::escape
            || (event->key == Key::character && event->codepoint == 'q'))
            co_return;
    }
}

} // namespace nxt::process_example

int main()
{
    return nxt::ui::run2(nxt::process_example::root);
}
