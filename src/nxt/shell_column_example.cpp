#include "nxt/tui.hpp"
#include "nxtio/app.hpp"
#include "nxtio/subprocess.hpp"
#include "nxtio/tui_subprocess.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nxt::shell_column_example {

using namespace nxt::tui;

inline constexpr auto shell_screen_height = 10 * ln;

struct State
{
    std::unique_ptr<nxt::subprocess::PtySession> shell;
    nxt::vterm::Terminal placeholder{1, 1};
    std::string status = "starting shell";
    std::uint64_t events_seen = 0;
    bool shell_focused = true;
    bool control_tap_active = false;
    bool control_tap_clean = false;
    std::optional<std::chrono::steady_clock::time_point> last_control_tap;
};

Size shell_size(nxt::ui::UIRuntime & runtime)
{
    return {runtime.terminal_width(), shell_screen_height};
}

std::string key_name(nxt::input::Key key)
{
    using nxt::input::Key;
    switch (key) {
    case Key::unknown:   return "unknown";
    case Key::character: return "char";
    case Key::enter:     return "enter";
    case Key::tab:       return "tab";
    case Key::backspace: return "bs";
    case Key::escape:    return "esc";
    case Key::insert:    return "ins";
    case Key::delete_:   return "del";
    case Key::left:      return "left";
    case Key::right:     return "right";
    case Key::up:        return "up";
    case Key::down:      return "down";
    case Key::home:      return "home";
    case Key::end:       return "end";
    case Key::page_up:   return "pgup";
    case Key::page_down: return "pgdn";
    case Key::begin:     return "begin";
    case Key::f1:        return "f1";
    case Key::f2:        return "f2";
    case Key::f3:        return "f3";
    case Key::f4:        return "f4";
    case Key::f5:        return "f5";
    case Key::f6:        return "f6";
    case Key::f7:        return "f7";
    case Key::f8:        return "f8";
    case Key::f9:        return "f9";
    case Key::f10:       return "f10";
    case Key::f11:       return "f11";
    case Key::f12:       return "f12";
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

std::string event_type_name(nxt::input::EventType type)
{
    using nxt::input::EventType;
    switch (type) {
    case EventType::press:   return "press";
    case EventType::repeat:  return "repeat";
    case EventType::release: return "release";
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

std::string event_line(
    std::uint64_t index,
    const nxt::input::KeyEvent & event,
    std::string_view encoded,
    std::string_view override_action = {})
{
    auto cp = event.codepoint == 0
        ? std::string{"-"}
        : std::format("U+{:04X}", event.codepoint);
    auto text = event.text.empty()
        ? std::string{"-"}
        : std::format("\"{}\"", escaped(event.text));
    auto action = !override_action.empty()
        ? std::string{override_action}
        : encoded.empty()
            ? std::string{"drop"}
            : std::format("send \"{}\"", escaped(encoded));
    auto detail = std::string{};
    if (event.shifted_codepoint || event.base_layout_codepoint) {
        auto shifted = event.shifted_codepoint
            ? std::format("U+{:04X}", *event.shifted_codepoint)
            : std::string{"-"};
        auto base = event.base_layout_codepoint
            ? std::format("U+{:04X}", *event.base_layout_codepoint)
            : std::string{"-"};
        detail = std::format(" shift/base {}/{}", shifted, base);
    }

    auto line = std::format(
        "{:04} {:<7} {:<7} {:<9} {:<7} {:<8} {}",
        index,
        event_type_name(event.type),
        key_name(event.key),
        modifiers(event.mods),
        cp,
        text,
        action);
    return line + detail;
}

bool is_kitty_control_key_event(const nxt::input::KeyEvent & event)
{
    if (event.key != nxt::input::Key::unknown)
        return false;

    // Kitty private-use codes for LEFT_CONTROL and RIGHT_CONTROL.
    return event.codepoint == 0xe062 || event.codepoint == 0xe068;
}

bool is_kitty_modifier_key_event(const nxt::input::KeyEvent & event)
{
    if (event.key != nxt::input::Key::unknown)
        return false;

    // Kitty reports bare modifier key presses with private-use key codes when
    // "report all keys" is enabled. They are useful for native shortcut
    // dispatch, but the shell bridge only cares about completed key chords.
    return event.codepoint >= 0xe061 && event.codepoint <= 0xe06e;
}

std::optional<std::string> update_shell_focus_escape(
    State & state,
    const nxt::input::KeyEvent & event)
{
    using namespace std::chrono_literals;

    if (is_kitty_control_key_event(event)) {
        if (event.type == nxt::input::EventType::press) {
            state.control_tap_active = true;
            state.control_tap_clean = true;
            return std::nullopt;
        }

        if (event.type != nxt::input::EventType::release)
            return std::nullopt;

        const auto was_clean = state.control_tap_active
            && state.control_tap_clean;
        state.control_tap_active = false;
        state.control_tap_clean = false;
        if (!was_clean)
            return std::nullopt;

        const auto now = std::chrono::steady_clock::now();
        if (state.last_control_tap
            && now - *state.last_control_tap <= 450ms) {
            state.last_control_tap.reset();
            state.shell_focused = !state.shell_focused;
            return std::format(
                "double-control focus -> {}",
                state.shell_focused ? "shell" : "nxt");
        }

        state.last_control_tap = now;
        return std::nullopt;
    }

    if (state.control_tap_active)
        state.control_tap_clean = false;
    return std::nullopt;
}

nxt::task<> forward_input(
    nxt::ui::UIRuntime & runtime,
    State & state,
    nxt::subprocess::PtySession & shell)
{
    while (!runtime.shutdown_requested()) {
        auto event = co_await runtime.next_input();
        if (!event)
            co_return;

        if (auto focus_change = update_shell_focus_escape(state, *event)) {
            ++state.events_seen;
            runtime.println(std::format(
                "{:04} {}",
                state.events_seen,
                *focus_change));
            runtime.signal_damage();
            continue;
        }

        auto encoded = shell.encode_key(*event);
        if (!is_kitty_modifier_key_event(*event)) {
            ++state.events_seen;
            runtime.println(event_line(
                state.events_seen,
                *event,
                encoded,
                state.shell_focused ? std::string_view{}
                                    : std::string_view{"captured"}));
        }
        if (state.shell_focused && !encoded.empty()) {
            co_await shell.write_all(
                runtime.scheduler(),
                std::move(encoded),
                runtime.get_stop_token());
        }
    }
}

nxt::task<> run_shell(nxt::ui::UIRuntime & runtime, State & state)
{
    state.shell =
        std::make_unique<nxt::subprocess::PtySession>(
            nxt::subprocess::PtySession::shell(shell_size(runtime)));
    state.status =
        std::format("shell pid {} TERM=xterm-256color",
                    state.shell->child_pid());
    runtime.signal_damage();

    auto reader = [&runtime, &state]() -> nxt::task<> {
        auto status = co_await state.shell->read_loop(
            runtime.scheduler(),
            runtime.get_stop_token(),
            [&runtime] { runtime.signal_damage(); });
        state.status = status.describe();
        runtime.signal_damage();
        runtime.request_shutdown();
    };

    auto input = [&runtime, &state]() -> nxt::task<> {
        co_await forward_input(runtime, state, *state.shell);
    };

    co_await nxt::when_all(reader(), input());
}

int main()
{
    return nxt::ui::run(
        State{},
        [](State & state) {
            auto header = styled_text(
                span("nxt shell ", fg(Rgba8::cyan()) | bold),
                span(state.status, fg(Rgba8::bright_black())),
                span(
                    state.shell_focused ? " focus:shell" : " focus:nxt",
                    state.shell_focused
                        ? fg(Rgba8::bright_green())
                        : fg(Rgba8::bright_yellow())));

            auto & terminal = state.shell ? state.shell->terminal()
                                          : state.placeholder;

            const auto hud_style =
                bg(Rgba8(7, 9, 12)) | fg(Rgba8::bright_white());
            const auto shell_style =
                bg(Rgba8(12, 14, 18)) | fg(Rgba8::white());

            return surface(
                hud_style,
                column(
                    header,
                    hrule(),
                    fixed_height(
                        shell_screen_height,
                        pty_screen(
                            state.shell.get(),
                            terminal,
                            shell_style))));
        },
        [](nxt::ui::UIRuntime & runtime, State & state) -> nxt::task<> {
            co_await run_shell(runtime, state);
        });
}

} // namespace nxt::shell_column_example

int main()
{
    return nxt::shell_column_example::main();
}
