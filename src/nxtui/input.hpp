#pragma once

#include <cstdint>
#include <memory>
#include <nxtui/units.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @namespace nxtui::input
 * Keyboard input decoding: raw terminal bytes in, `KeyEvent` values out.
 *
 * `InputModeGuard` puts the terminal in raw mode and enables the Kitty
 * keyboard protocol; `Parser` turns the bytes read from stdin into events,
 * handling legacy control bytes, UTF-8 text, CSI cursor and function keys,
 * Kitty `CSI ... u` key reports with modifiers and event types, and cursor
 * position reports. Mouse reports are not decoded.
 */
namespace nxtui::input {

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
///
/// `key` says what was pressed. For `Key::character`, `codepoint` is the
/// key's Unicode codepoint (lowercase `c` for Ctrl-C, with `mods.ctrl`) and
/// `text` is what it would insert. Use `is_text()` to decide whether to
/// insert `text`. A cursor position report arrives as an event with
/// `key == Key::unknown` and `cursor_position` set. Unrecognized sequences
/// also arrive as `Key::unknown`, with their bytes in `raw`.
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
    /// Cursor position report from DSR 6: CSI row ; col R.
    std::optional<Pos> cursor_position;

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

    /// True when this event is a terminal cursor-position report.
    [[nodiscard]] bool is_cursor_position_report() const noexcept
    {
        return cursor_position.has_value();
    }

    /// True when the event is plain Ctrl-L.
    [[nodiscard]] bool is_ctrl_l() const noexcept
    {
        return key == Key::character && type != EventType::release
            && codepoint == static_cast<std::uint32_t>('l') && mods.ctrl
            && !mods.alt && !mods.super && !mods.hyper && !mods.meta;
    }

    /// True when the event is plain Ctrl-Z.
    [[nodiscard]] bool is_ctrl_z() const noexcept
    {
        return key == Key::character && type != EventType::release
            && codepoint == static_cast<std::uint32_t>('z') && mods.ctrl
            && !mods.alt && !mods.super && !mods.hyper && !mods.meta;
    }
};

/// Incremental decoder for terminal keyboard input.
///
/// Feed it whatever `read` returned; incomplete sequences are kept for the
/// next call. A lone ESC is ambiguous (the Escape key or the start of a
/// sequence), so it stays pending: call `flush()` after a short quiet
/// period to deliver it. ESC followed by anything other than `[` is
/// delivered as `Key::escape` followed by the next byte's own event (no
/// Alt+key folding). Malformed CSI sequences become `Key::unknown` events.
class Parser
{
public:
    /// Append `bytes` and return every event that is now complete, in
    /// order.
    [[nodiscard]] std::vector<KeyEvent> feed(std::string_view bytes);
    /// Deliver all pending bytes as events now, one per byte: ESC as
    /// `Key::escape`, control bytes as usual, anything else (such as a
    /// partial UTF-8 sequence) as `Key::unknown`.
    [[nodiscard]] std::vector<KeyEvent> flush();
    /// True when bytes are held back waiting for the rest of a sequence.
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

/// RAII guard that puts stdin into the raw input mode used by the TUI and
/// enables the Kitty keyboard protocol.
///
/// Does nothing unless both stdin and stdout are terminals. Otherwise it
/// disables echo, canonical mode, and input translation on stdin (keeping
/// `ISIG`, so a legacy Ctrl-C byte still raises `SIGINT`), makes reads
/// non-blocking at the termios level (`VMIN = VTIME = 0`), and pushes Kitty
/// keyboard flags 31 (disambiguate, event types, alternate keys, all keys
/// as escape codes, associated text) to stdout. The destructor pops the
/// keyboard mode and restores the saved termios.
class InputModeGuard
{
public:
    /// Enter raw mode and push the Kitty keyboard mode, if on a terminal.
    InputModeGuard();
    /// Pop the keyboard mode and restore the original terminal settings.
    ~InputModeGuard();

    InputModeGuard(const InputModeGuard &) = delete;
    InputModeGuard & operator=(const InputModeGuard &) = delete;

    /// True when both stdin and stdout are terminals and the keyboard mode
    /// was pushed. Raw termios mode is attempted but its failure is not
    /// reported here.
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

} // namespace nxtui::input
