#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::input {

/// Normalized key identity after terminal escape-sequence decoding.
enum class Key {
    unknown,
    character,
    enter,
    tab,
    backspace,
    escape,
    insert,
    delete_,
    left,
    right,
    up,
    down,
    home,
    end,
    page_up,
    page_down,
    begin,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
};

/// Key event phase. Terminals that do not report releases use `press`.
enum class EventType {
    press,
    repeat,
    release,
};

/// Modifier set decoded from terminal keyboard protocols.
struct Modifiers
{
    /// Shift modifier.
    bool shift = false;
    /// Alt/Option modifier.
    bool alt = false;
    /// Control modifier.
    bool ctrl = false;
    /// Super/Command/Windows modifier.
    bool super = false;
    /// Hyper modifier.
    bool hyper = false;
    /// Meta modifier.
    bool meta = false;
    /// Caps Lock state when reported.
    bool caps_lock = false;
    /// Num Lock state when reported.
    bool num_lock = false;

    friend bool operator==(const Modifiers &, const Modifiers &) = default;
};

/// One decoded keyboard input event.
struct KeyEvent
{
    /// Logical key identity.
    Key key = Key::unknown;
    /// Press/repeat/release phase.
    EventType type = EventType::press;
    /// Active modifier set.
    Modifiers mods{};
    /// Unicode codepoint for character keys, or 0 when absent.
    std::uint32_t codepoint = 0;
    /// Shifted Unicode codepoint from keyboard protocol metadata.
    std::optional<std::uint32_t> shifted_codepoint;
    /// Base-layout Unicode codepoint from keyboard protocol metadata.
    std::optional<std::uint32_t> base_layout_codepoint;
    /// Text to insert for plain text events.
    std::string text;
    /// Raw bytes consumed to produce this event.
    std::string raw;

    /// True when the event should be treated as ordinary text insertion.
    [[nodiscard]] bool is_text() const noexcept
    {
        return key == Key::character && type != EventType::release
            && !text.empty()
            && !mods.alt && !mods.ctrl && !mods.super && !mods.hyper
            && !mods.meta;
    }

    /// True when the event is plain Ctrl-C.
    [[nodiscard]] bool is_ctrl_c() const noexcept
    {
        return key == Key::character && type != EventType::release
            && codepoint == static_cast<std::uint32_t>('c') && mods.ctrl
            && !mods.alt && !mods.super && !mods.hyper && !mods.meta;
    }
};

/// Incremental parser for terminal keyboard input bytes.
class Parser
{
public:
    /// Feed bytes and return all complete decoded key events.
    [[nodiscard]] std::vector<KeyEvent> feed(std::string_view bytes);
    /// Flush pending bytes as best-effort events.
    [[nodiscard]] std::vector<KeyEvent> flush();
    /// True when the parser is waiting for more bytes.
    [[nodiscard]] bool has_pending() const noexcept
    {
        return !pending_.empty();
    }

private:
    enum class CsiResult {
        incomplete,
        parsed,
        invalid,
    };

    [[nodiscard]] CsiResult parse_csi(std::size_t end, KeyEvent & event);
    [[nodiscard]] bool parse_control(unsigned char c, KeyEvent & event);
    [[nodiscard]] bool parse_utf8(KeyEvent & event);

    std::string pending_;
};

/// RAII guard that puts stdin into the raw input mode used by the TUI.
class InputModeGuard
{
public:
    /// Enter raw mode when possible.
    InputModeGuard();
    /// Restore the original terminal mode.
    ~InputModeGuard();

    InputModeGuard(const InputModeGuard &) = delete;
    InputModeGuard & operator=(const InputModeGuard &) = delete;

    /// True when raw mode was successfully enabled.
    [[nodiscard]] bool enabled() const noexcept
    {
        return enabled_;
    }

private:
    bool enabled_ = false;
    bool termios_saved_ = false;
    struct TermiosStorage;
    std::unique_ptr<TermiosStorage> termios_;
};

} // namespace nxt::input
