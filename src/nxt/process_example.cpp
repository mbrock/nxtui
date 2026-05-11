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
    yard & self, std::string a, std::string b, Rgba8 color)
{
    int i = 0;
    while (!self.cancelled()) {
        self.draw(text(i % 2 == 0 ? a : b, fg(color)));
        co_await self.sleep(250ms);
        ++i;
    }
}

nxt::task<> root(yard & self)
{
    auto left = self.spawn([](yard & s) {
        return animator(s, "<<<", ">>>", Rgba8::cyan());
    });
    auto right = self.spawn([](yard & s) {
        return animator(s, "***", "...", Rgba8::yellow());
    });

    self.draw(column(
        row(
            left.surface(),
            text(std::string{"  "}),
            right.surface()),
        text(std::string{"press q or esc to quit"},
             fg(Rgba8::white()))));

    co_await next_key_press(self, is_quit_key);
}

} // namespace nxt::process_example

int main()
{
    return nxt::ui::run2(nxt::process_example::root);
}
