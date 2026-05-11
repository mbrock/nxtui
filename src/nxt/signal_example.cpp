#include "nxt/any_layout.hpp"
#include "nxt/signal.hpp"
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

nxt::task<> root(Self & self)
{
    auto yes = self.signal<KeyPress>();
    auto no = self.signal<KeyPress>();
    auto quit = self.signal<KeyPress>();

    auto in = self.spawn(
        [yes = yes.publisher(),
         no = no.publisher(),
         quit = quit.publisher()](Self & s) -> nxt::task<> {
            using nxt::input::Key;
            while (!s.cancelled()) {
                auto ev = co_await s.next_input();
                if (!ev)
                    co_return;

                if (ev->type != input::EventType::release)
                  continue;

                try {
                    if (ev->key == Key::escape
                        || (ev->key == Key::character
                            && ev->codepoint == 'q'))
                    {
                        co_await quit.push({});
                    } else if (
                        ev->key == Key::character && ev->codepoint == 'y')
                    {
                        co_await yes.push({});
                    } else if (
                        ev->key == Key::character && ev->codepoint == 'n')
                    {
                        co_await no.push({});
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

        auto r = co_await self.select(yes, no, quit);

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
