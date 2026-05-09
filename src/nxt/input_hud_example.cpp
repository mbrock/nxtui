#include "nxt/baltics.hpp"
#include "nxt/text_field.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"
#include "nxtio/app.hpp"
#include "nxtio/async.hpp"
#include "nxtio/input.hpp"
#include "nxtio/text_field.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace nxt::input_hud_example {

using namespace nxt::tui;

struct State
{
    TextField field;
    std::uint64_t events_seen = 0;
};

std::string key_name(nxt::input::Key key)
{
    using nxt::input::Key;
    switch (key) {
    case Key::unknown:    return "unknown";
    case Key::character:  return "char";
    case Key::enter:      return "enter";
    case Key::tab:        return "tab";
    case Key::backspace:  return "bs";
    case Key::escape:     return "esc";
    case Key::insert:     return "ins";
    case Key::delete_:    return "del";
    case Key::left:       return "left";
    case Key::right:      return "right";
    case Key::up:         return "up";
    case Key::down:       return "down";
    case Key::home:       return "home";
    case Key::end:        return "end";
    case Key::page_up:    return "pgup";
    case Key::page_down:  return "pgdn";
    case Key::begin:      return "begin";
    case Key::f1:         return "f1";
    case Key::f2:         return "f2";
    case Key::f3:         return "f3";
    case Key::f4:         return "f4";
    case Key::f5:         return "f5";
    case Key::f6:         return "f6";
    case Key::f7:         return "f7";
    case Key::f8:         return "f8";
    case Key::f9:         return "f9";
    case Key::f10:        return "f10";
    case Key::f11:        return "f11";
    case Key::f12:        return "f12";
    }
    return "invalid";
}

std::string modifiers(nxt::input::Modifiers mods)
{
    std::string out;
    auto add = [&](std::string_view name, bool enabled) {
        if (!enabled)
            return;
        if (!out.empty())
            out += '+';
        out += name;
    };

    add("S", mods.shift);
    add("A", mods.alt);
    add("C", mods.ctrl);
    add("Su", mods.super);
    add("H", mods.hyper);
    add("M", mods.meta);
    add("Caps", mods.caps_lock);
    add("Num", mods.num_lock);
    return out.empty() ? "-" : out;
}

std::string event_type_symbol(nxt::input::EventType type)
{
    using nxt::input::EventType;
    switch (type) {
    case EventType::press:   return "↑";
    case EventType::repeat:  return "↻";
    case EventType::release: return "↓";
    }
    return "?";
}

std::string escaped(std::string_view bytes)
{
    std::string out;
    for (unsigned char c : bytes) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case 0x1b: out += "\\e"; break;
        default:
            if (c < 0x20 || c == 0x7f)
                out += std::format("\\x{:02X}", static_cast<unsigned>(c));
            else
                out.push_back(static_cast<char>(c));
            break;
        }
    }
    return out;
}

std::string event_line(std::uint64_t index, const nxt::input::KeyEvent & event)
{
    auto cp = event.codepoint == 0
        ? std::string{"-"}
        : std::format("U+{:04X}", event.codepoint);
    auto shifted = event.shifted_codepoint
        ? std::format("U+{:04X}", *event.shifted_codepoint)
        : std::string{"-"};
    auto base = event.base_layout_codepoint
        ? std::format("U+{:04X}", *event.base_layout_codepoint)
        : std::string{"-"};
    auto text = event.text.empty()
        ? std::string{"-"}
        : std::format("\"{}\"", escaped(event.text));
    auto line = std::format(
        "{:04} {} {:<7} {:<9} {:<7} {:<8} {}",
        index,
        event_type_symbol(event.type),
        key_name(event.key),
        modifiers(event.mods),
        cp,
        text,
        escaped(event.raw));
    if (event.shifted_codepoint || event.base_layout_codepoint)
        line += std::format("  {}/{}", shifted, base);
    return line;
}

auto input_hud(const State & state)
{
    const auto p = theme::baltic_church;
    return text_field(
        state.field,
        {
            .placeholder = "type to test the input parser",
            .style = {
                .fg             = Rgba8{p.fg, 255},
                .bg             = Rgba8{p.bg_elev, 255},
                .prefix_fg      = Rgba8{p.cyan, 255},
                .placeholder_fg = Rgba8{p.fg_subtle, 255},
            },
        });
}

nxt::task<> update(nxt::ui::UIRuntime & runtime, State & state)
{
    while (!runtime.shutdown_requested()) {
        auto event = co_await runtime.next_input();
        if (!event)
            co_return;

        ++state.events_seen;
        runtime.println(event_line(state.events_seen, *event));

        if (apply_key(state.field, *event))
            runtime.signal_damage();

        if (event->key == nxt::input::Key::escape)
            runtime.request_shutdown();
    }
}

int run()
{
    return nxt::ui::run(
        State{},
        [](const State & state) { return input_hud(state); },
        update);
}

} // namespace nxt::input_hud_example

int main()
{
    return nxt::input_hud_example::run();
}
