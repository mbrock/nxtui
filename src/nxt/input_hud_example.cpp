#include "nxt/tui.hpp"
#include "nxt/units.hpp"
#include "nxtio/app.hpp"
#include "nxtio/async.hpp"
#include "nxtio/input.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace nxt::input_hud_example {

using namespace nxt::tui;

struct State
{
    std::string text;
    std::size_t cursor_byte = 0;
    std::uint64_t events_seen = 0;
};

std::string key_name(nxt::input::Key key)
{
    using nxt::input::Key;
    switch (key) {
    case Key::unknown:
        return "unknown";
    case Key::character:
        return "char";
    case Key::enter:
        return "enter";
    case Key::tab:
        return "tab";
    case Key::backspace:
        return "bs";
    case Key::escape:
        return "esc";
    case Key::insert:
        return "ins";
    case Key::delete_:
        return "del";
    case Key::left:
        return "left";
    case Key::right:
        return "right";
    case Key::up:
        return "up";
    case Key::down:
        return "down";
    case Key::home:
        return "home";
    case Key::end:
        return "end";
    case Key::page_up:
        return "pgup";
    case Key::page_down:
        return "pgdn";
    case Key::begin:
        return "begin";
    case Key::f1:
        return "f1";
    case Key::f2:
        return "f2";
    case Key::f3:
        return "f3";
    case Key::f4:
        return "f4";
    case Key::f5:
        return "f5";
    case Key::f6:
        return "f6";
    case Key::f7:
        return "f7";
    case Key::f8:
        return "f8";
    case Key::f9:
        return "f9";
    case Key::f10:
        return "f10";
    case Key::f11:
        return "f11";
    case Key::f12:
        return "f12";
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
    case EventType::press:
        return "↑";
    case EventType::repeat:
        return "↻";
    case EventType::release:
        return "↓";
    }
    return "?";
}

std::string escaped(std::string_view bytes)
{
    std::string out;
    for (unsigned char c : bytes) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case 0x1b:
            out += "\\e";
            break;
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

std::size_t next_codepoint(std::string_view text, std::size_t byte)
{
    if (byte >= text.size())
        return text.size();
    ++byte;
    while (byte < text.size()
           && (static_cast<unsigned char>(text[byte]) & 0xc0) == 0x80)
        ++byte;
    return byte;
}

std::size_t previous_codepoint(std::string_view text, std::size_t byte)
{
    if (byte == 0)
        return 0;
    --byte;
    while (byte > 0
           && (static_cast<unsigned char>(text[byte]) & 0xc0) == 0x80)
        --byte;
    return byte;
}

std::size_t codepoint_count(std::string_view text)
{
    std::size_t count = 0;
    for (std::size_t byte = 0; byte < text.size();
         byte = next_codepoint(text, byte))
        ++count;
    return count;
}

std::size_t byte_at_cell(std::string_view text, std::size_t cell)
{
    std::size_t byte = 0;
    for (std::size_t i = 0; i < cell && byte < text.size(); ++i)
        byte = next_codepoint(text, byte);
    return byte;
}

std::size_t cell_at_byte(std::string_view text, std::size_t byte)
{
    byte = std::min(byte, text.size());
    std::size_t cell = 0;
    for (std::size_t i = 0; i < byte; i = next_codepoint(text, i))
        ++cell;
    return cell;
}

void apply_event(State & state, const nxt::input::KeyEvent & event)
{
    using nxt::input::Key;

    state.cursor_byte = std::min(state.cursor_byte, state.text.size());

    if (event.is_text()) {
        state.text.insert(state.cursor_byte, event.text);
        state.cursor_byte += event.text.size();
        return;
    }

    switch (event.key) {
    case Key::backspace:
        if (state.cursor_byte > 0) {
            auto prev = previous_codepoint(state.text, state.cursor_byte);
            state.text.erase(prev, state.cursor_byte - prev);
            state.cursor_byte = prev;
        }
        break;
    case Key::delete_:
        if (state.cursor_byte < state.text.size()) {
            auto next = next_codepoint(state.text, state.cursor_byte);
            state.text.erase(state.cursor_byte, next - state.cursor_byte);
        }
        break;
    case Key::left:
        state.cursor_byte = previous_codepoint(state.text, state.cursor_byte);
        break;
    case Key::right:
        state.cursor_byte = next_codepoint(state.text, state.cursor_byte);
        break;
    case Key::home:
        state.cursor_byte = 0;
        break;
    case Key::end:
        state.cursor_byte = state.text.size();
        break;
    default:
        break;
    }
}

auto input_hud(const State & state)
{
    auto text = state.text;
    auto cursor_byte = state.cursor_byte;
    auto events_seen = state.events_seen;

    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size size) {
            if (size.w.count() == 0)
                return;

            auto field_w = size.w.count() > 2 ? size.w.count() - 2 : 0;
            auto cursor_cell = cell_at_byte(text, cursor_byte);
            auto total_cells = codepoint_count(text);
            auto scroll_cell = cursor_cell >= field_w && field_w > 0
                ? cursor_cell - field_w + 1
                : std::size_t{0};
            auto start_byte = byte_at_cell(text, scroll_cell);
            auto end_byte =
                byte_at_cell(std::string_view{text}.substr(start_byte), field_w)
                + start_byte;
            auto visible = text.substr(start_byte, end_byte - start_byte);
            auto visible_cursor = cursor_cell - scroll_cell;

            r.write_text(Pos::at(0 * ch, 0 * ln), " ");
            r.write_text(Pos::at(1 * ch, 0 * ln), visible);

            for (std::size_t x = 0; x < size.w.count(); ++x)
                r.set_bg(Pos::at(x * ch, 0 * ln), Rgba8(42, 47, 54));

            auto cursor_x = std::min(visible_cursor + 1, size.w.count() - 1);
            r.set_em(Pos::at(cursor_x * ch, 0 * ln), Emphasis::reverse);
            if (cursor_cell == total_cells)
                r.set_char(Pos::at(cursor_x * ch, 0 * ln), ' ');

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
        apply_event(state, *event);
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
