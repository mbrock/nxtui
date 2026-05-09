#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nxtio/async.hpp"

namespace nxt::input {

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

enum class EventType {
    press,
    repeat,
    release,
};

struct Modifiers
{
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
    bool super = false;
    bool hyper = false;
    bool meta = false;
    bool caps_lock = false;
    bool num_lock = false;

    friend bool operator==(const Modifiers &, const Modifiers &) = default;
};

struct KeyEvent
{
    Key key = Key::unknown;
    EventType type = EventType::press;
    Modifiers mods{};
    std::uint32_t codepoint = 0;
    std::optional<std::uint32_t> shifted_codepoint;
    std::optional<std::uint32_t> base_layout_codepoint;
    std::string text;
    std::string raw;

    [[nodiscard]] bool is_text() const noexcept
    {
        return key == Key::character && type != EventType::release
            && !text.empty()
            && !mods.alt && !mods.ctrl && !mods.super && !mods.hyper
            && !mods.meta;
    }

    [[nodiscard]] bool is_ctrl_c() const noexcept
    {
        return key == Key::character && type != EventType::release
            && codepoint == static_cast<std::uint32_t>('c') && mods.ctrl
            && !mods.alt && !mods.super && !mods.hyper && !mods.meta;
    }
};

class Parser
{
public:
    [[nodiscard]] std::vector<KeyEvent> feed(std::string_view bytes);
    [[nodiscard]] std::vector<KeyEvent> flush();
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

class InputModeGuard
{
public:
    InputModeGuard();
    ~InputModeGuard();

    InputModeGuard(const InputModeGuard &) = delete;
    InputModeGuard & operator=(const InputModeGuard &) = delete;

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
