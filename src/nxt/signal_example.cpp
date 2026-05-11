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
#include <variant>

namespace nxt::signal_example {

using namespace nxt::tui;
using namespace nxt::ui;

using KeyPress = std::monostate;

nxt::task<> root(yard & self)
{
    auto yes = nxt::signal<KeyPress>{};
    auto no = nxt::signal<KeyPress>{};
    auto quit = nxt::signal<KeyPress>{};

    auto in = self.spawn(
        [yes = nxt::publisher_for(yes),
         no = nxt::publisher_for(no),
         quit = nxt::publisher_for(quit)](yard & s) -> nxt::task<> {
            while (!s.cancelled()) {
                auto ev = co_await next_key_press(s, [](const auto & ev) {
                    return is_quit_key(ev)
                        || is_character(ev, 'y')
                        || is_character(ev, 'n');
                });
                if (!ev)
                    co_return;

                try {
                    if (is_quit_key(*ev)) {
                        co_await nxt::publish(quit, {});
                    } else if (is_character(*ev, 'y')) {
                        co_await nxt::publish(yes, {});
                    } else if (is_character(*ev, 'n')) {
                        co_await nxt::publish(no, {});
                    }
                } catch (const nxt::disconnected &) {
                    co_return;
                }
            }
        });

    while (!self.cancelled()) {
        self.draw(fixed_height(
            1 * ln,
            column(text(
                std::string{"press y, n, or q"}, fg(Rgba8::white())))));

        auto r = co_await nxt::select(self.scope(), yes, no, quit);

        if (r.index() == 2 || !std::visit(
                [](const auto & press) { return press.has_value(); }, r))
        {
            co_return;
        }

        auto message = r.index() == 0
            ? std::string{"-> yes!"}
            : std::string{"-> no."};

        self.println(r.index() == 0 ? "yes" : "no");
        self.draw(text(
            message,
            fg(r.index() == 0 ? Rgba8::green() : Rgba8::yellow())));
        co_await self.sleep(std::chrono::milliseconds(200));
    }
}

} // namespace nxt::signal_example

int main()
{
    return nxt::ui::run2(nxt::signal_example::root);
}
