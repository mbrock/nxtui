#include "nxt/slot.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"
#include "nxtio/app.hpp"
#include "nxtio/async.hpp"
#include "nxtio/input.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace nxt::slot_example {

using namespace nxt::tui;
using namespace std::chrono_literals;

using TextLeaf = decltype(text(std::string{}, Style{}));

struct State
{
    Slot<TextLeaf> spinner{text(std::string{"."}, fg(Rgba8::cyan()))};
    Slot<TextLeaf> status{
        text(std::string{"press q or esc to quit"}, fg(Rgba8::white()))};
};

auto build_ui(const State & state)
{
    return column(
        row(
            state.spinner,
            text(std::string{"  loading..."}, fg(Rgba8::white()))),
        state.status);
}

nxt::task<> run_spinner(nxt::ui::UIRuntime & runtime, const Slot<TextLeaf> & slot)
{
    static constexpr const char * frames[] = {
        "⡀", "⡄", "⡆", "⡇",
        "⠏", "⠋", "⠉", "⠈",
    };
    int i = 0;
    while (!runtime.shutdown_requested()) {
        slot.publish(text(std::string{frames[i]}, fg(Rgba8::cyan())));
        co_await runtime.sleep(100ms);
        i = (i + 1) % 8;
    }
}

nxt::task<> handle_input(nxt::ui::UIRuntime & runtime, State & state)
{
    using nxt::input::Key;
    while (!runtime.shutdown_requested()) {
        auto event = co_await runtime.next_input();
        if (!event)
            co_return;

        if (event->key == Key::escape
            || (event->key == Key::character && event->codepoint == 'q'))
        {
            state.status.publish(
                text(std::string{"bye!"}, fg(Rgba8::yellow())));
            co_await runtime.sleep(150ms);
            runtime.request_shutdown();
        }
    }
}

nxt::task<> update(nxt::ui::UIRuntime & runtime, State & state)
{
    auto damage = [&runtime] { runtime.signal_damage(); };
    state.spinner.set_on_publish(damage);
    state.status.set_on_publish(damage);

    std::vector<nxt::task<>> tasks;
    tasks.push_back(run_spinner(runtime, state.spinner));
    tasks.push_back(handle_input(runtime, state));
    co_await nxt::when_all(std::move(tasks));
}

int run()
{
    return nxt::ui::run(
        State{},
        [](const State & state) { return build_ui(state); },
        update);
}

} // namespace nxt::slot_example

int main()
{
    return nxt::slot_example::run();
}
