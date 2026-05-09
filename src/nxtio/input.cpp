#include "nxtio/input.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <sys/termios.h>
#include <unistd.h>

namespace nxt::input {
namespace {

// Enable Kitty keyboard protocol flags:
//   1  disambiguate escape codes
//   2  report press/repeat/release
//   4  report alternate key codes
//   8  report all keys as CSI u
//   16 report associated text
constexpr std::string_view kitty_push_structured_keyboard = "\x1b[>31u";
constexpr std::string_view kitty_pop_keyboard_mode = "\x1b[<u";

std::string encode_utf8(std::uint32_t cp)
{
    std::string out;
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    return out;
}

std::optional<long> parse_number(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return value;
}

std::vector<std::string_view> split(std::string_view body, char separator)
{
    std::vector<std::string_view> out;
    while (true) {
        auto pos = body.find(separator);
        out.push_back(body.substr(0, pos));
        if (pos == std::string_view::npos)
            break;
        body.remove_prefix(pos + 1);
    }
    return out;
}

Modifiers decode_modifiers(long encoded)
{
    // Kitty follows the xterm convention: encoded value is bitmask + 1.
    auto bits = encoded > 0 ? encoded - 1 : 0;
    return Modifiers{
        .shift = (bits & 1) != 0,
        .alt = (bits & 2) != 0,
        .ctrl = (bits & 4) != 0,
        .super = (bits & 8) != 0,
        .hyper = (bits & 16) != 0,
        .meta = (bits & 32) != 0,
        .caps_lock = (bits & 64) != 0,
        .num_lock = (bits & 128) != 0,
    };
}

EventType decode_event_type(long encoded)
{
    switch (encoded) {
    case 2:
        return EventType::repeat;
    case 3:
        return EventType::release;
    default:
        return EventType::press;
    }
}

std::optional<std::uint32_t> parse_codepoint(std::string_view text)
{
    auto value = parse_number(text);
    if (!value || *value < 0 || *value > 0x10ffff)
        return std::nullopt;
    if (*value >= 0xd800 && *value <= 0xdfff)
        return std::nullopt;
    return static_cast<std::uint32_t>(*value);
}

std::optional<std::string> parse_associated_text(std::string_view text)
{
    std::string out;
    if (text.empty())
        return out;

    for (auto part : split(text, ':')) {
        auto cp = parse_codepoint(part);
        if (!cp)
            return std::nullopt;
        out += encode_utf8(*cp);
    }

    return out;
}

Key key_from_tilde(long number)
{
    switch (number) {
    case 2:
        return Key::insert;
    case 3:
        return Key::delete_;
    case 5:
        return Key::page_up;
    case 6:
        return Key::page_down;
    case 7:
        return Key::home;
    case 8:
        return Key::end;
    case 11:
        return Key::f1;
    case 12:
        return Key::f2;
    case 13:
        return Key::f3;
    case 14:
        return Key::f4;
    case 15:
        return Key::f5;
    case 17:
        return Key::f6;
    case 18:
        return Key::f7;
    case 19:
        return Key::f8;
    case 20:
        return Key::f9;
    case 21:
        return Key::f10;
    case 23:
        return Key::f11;
    case 24:
        return Key::f12;
    default:
        return Key::unknown;
    }
}

Key key_from_final(char command)
{
    switch (command) {
    case 'A':
        return Key::up;
    case 'B':
        return Key::down;
    case 'C':
        return Key::right;
    case 'D':
        return Key::left;
    case 'E':
        return Key::begin;
    case 'F':
        return Key::end;
    case 'H':
        return Key::home;
    case 'P':
        return Key::f1;
    case 'Q':
        return Key::f2;
    case 'R':
        return Key::f3;
    case 'S':
        return Key::f4;
    case 'Z':
        return Key::tab;
    default:
        return Key::unknown;
    }
}

Key key_from_codepoint(std::uint32_t cp)
{
    switch (cp) {
    case 0:
        return Key::unknown;
    case 9:
        return Key::tab;
    case 13:
        return Key::enter;
    case 27:
        return Key::escape;
    case 127:
        return Key::backspace;
    default:
        if (cp >= 0xe000 && cp <= 0xf8ff)
            return Key::unknown;
        return Key::character;
    }
}

bool valid_utf8_cont(unsigned char c)
{
    return (c & 0xc0) == 0x80;
}

} // namespace

std::vector<KeyEvent> Parser::feed(std::string_view bytes)
{
    pending_.append(bytes);

    std::vector<KeyEvent> events;
    while (!pending_.empty()) {
        KeyEvent event;
        auto c = static_cast<unsigned char>(pending_.front());

        if (c == 0x1b) {
            if (pending_.size() == 1)
                break;

            if (pending_[1] == '[') {
                auto end = std::string::npos;
                for (std::size_t i = 2; i < pending_.size(); ++i) {
                    auto b = static_cast<unsigned char>(pending_[i]);
                    if (b >= 0x40 && b <= 0x7e) {
                        end = i;
                        break;
                    }
                }

                if (end == std::string::npos)
                    break;

                switch (parse_csi(end, event)) {
                case CsiResult::parsed:
                    events.push_back(std::move(event));
                    pending_.erase(0, end + 1);
                    continue;
                case CsiResult::invalid:
                    event.key = Key::unknown;
                    event.raw = pending_.substr(0, end + 1);
                    events.push_back(std::move(event));
                    pending_.erase(0, end + 1);
                    continue;
                case CsiResult::incomplete:
                    break;
                }
            }

            event.key = Key::escape;
            event.raw = pending_.substr(0, 1);
            events.push_back(std::move(event));
            pending_.erase(0, 1);
            continue;
        }

        if (c < 0x20 || c == 0x7f) {
            if (parse_control(c, event)) {
                events.push_back(std::move(event));
                pending_.erase(0, 1);
                continue;
            }
        }

        if (!parse_utf8(event))
            break;

        events.push_back(std::move(event));
    }

    return events;
}

std::vector<KeyEvent> Parser::flush()
{
    std::vector<KeyEvent> events;
    while (!pending_.empty()) {
        KeyEvent event;
        auto c = static_cast<unsigned char>(pending_.front());
        if (c == 0x1b) {
            event.key = Key::escape;
            event.raw = pending_.substr(0, 1);
            pending_.erase(0, 1);
        } else if (c < 0x20 || c == 0x7f) {
            (void) parse_control(c, event);
            pending_.erase(0, 1);
        } else {
            event.key = Key::unknown;
            event.raw = pending_.substr(0, 1);
            pending_.erase(0, 1);
        }
        events.push_back(std::move(event));
    }
    return events;
}

Parser::CsiResult Parser::parse_csi(std::size_t end, KeyEvent & event)
{
    auto raw = pending_.substr(0, end + 1);
    auto body = std::string_view{pending_}.substr(2, end - 2);
    auto command = pending_[end];

    if (!body.empty() && body.front() == '<')
        return CsiResult::invalid; // Mouse input is intentionally out of scope here.

    auto params = split(body, ';');
    auto key_parts = params.empty() ? std::vector<std::string_view>{}
                                    : split(params[0], ':');
    auto first = key_parts.empty() || key_parts[0].empty()
        ? std::optional<std::uint32_t>{}
        : parse_codepoint(key_parts[0]);
    if (!first && command != 'Z')
        return CsiResult::invalid;

    event.raw = std::move(raw);
    if (key_parts.size() >= 2 && !key_parts[1].empty()) {
        auto shifted = parse_codepoint(key_parts[1]);
        if (!shifted)
            return CsiResult::invalid;
        event.shifted_codepoint = *shifted;
    }
    if (key_parts.size() >= 3 && !key_parts[2].empty()) {
        auto base = parse_codepoint(key_parts[2]);
        if (!base)
            return CsiResult::invalid;
        event.base_layout_codepoint = *base;
    }

    if (params.size() >= 2) {
        auto modifier_parts = split(params[1], ':');
        if (!modifier_parts.empty()) {
            if (auto mods = parse_number(modifier_parts[0]))
                event.mods = decode_modifiers(*mods);
        }
        if (modifier_parts.size() >= 2 && !modifier_parts[1].empty()) {
            auto type = parse_number(modifier_parts[1]);
            if (!type)
                return CsiResult::invalid;
            event.type = decode_event_type(*type);
        }
    }

    if (params.size() >= 3) {
        // In the Kitty protocol the associated text parameter is the third
        // parameter. Accept later non-empty fields as a defensive fallback for
        // terminals that emit extra empty parameters before the text.
        auto associated_text = params[2];
        for (std::size_t i = 3; associated_text.empty() && i < params.size();
             ++i)
            associated_text = params[i];

        if (!associated_text.empty()) {
            auto text = parse_associated_text(associated_text);
            if (!text)
                return CsiResult::invalid;
            event.text = std::move(*text);
        }
    }

    if (command == 'u') {
        auto cp = *first;
        event.codepoint = cp;
        event.key = key_from_codepoint(cp);
        if (!event.text.empty() && event.key == Key::unknown)
            event.key = Key::character;
        if (event.key == Key::character && event.text.empty())
            event.text = encode_utf8(cp);
        return CsiResult::parsed;
    }

    if (command == '~') {
        event.key = key_from_tilde(*first);
        return CsiResult::parsed;
    }

    event.key = key_from_final(command);
    if (command == 'Z')
        event.mods.shift = true;
    return CsiResult::parsed;
}

bool Parser::parse_control(unsigned char c, KeyEvent & event)
{
    event.raw.assign(1, static_cast<char>(c));

    switch (c) {
    case '\r':
    case '\n':
        event.key = Key::enter;
        return true;
    case '\t':
        event.key = Key::tab;
        return true;
    case '\b':
    case 0x7f:
        event.key = Key::backspace;
        return true;
    default:
        break;
    }

    if (c >= 1 && c <= 26) {
        event.key = Key::character;
        event.mods.ctrl = true;
        event.codepoint = static_cast<std::uint32_t>('a' + c - 1);
        event.text = encode_utf8(event.codepoint);
        return true;
    }

    event.key = Key::unknown;
    return true;
}

bool Parser::parse_utf8(KeyEvent & event)
{
    auto first = static_cast<unsigned char>(pending_.front());
    std::size_t len = 1;
    std::uint32_t cp = 0;

    if ((first & 0x80) == 0) {
        cp = first;
    } else if ((first & 0xe0) == 0xc0) {
        len = 2;
        cp = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        len = 3;
        cp = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        len = 4;
        cp = first & 0x07;
    } else {
        event.key = Key::unknown;
        event.raw = pending_.substr(0, 1);
        pending_.erase(0, 1);
        return true;
    }

    if (pending_.size() < len)
        return false;

    for (std::size_t i = 1; i < len; ++i) {
        auto c = static_cast<unsigned char>(pending_[i]);
        if (!valid_utf8_cont(c)) {
            event.key = Key::unknown;
            event.raw = pending_.substr(0, 1);
            pending_.erase(0, 1);
            return true;
        }
        cp = (cp << 6) | (c & 0x3f);
    }

    event.key = Key::character;
    event.codepoint = cp;
    event.text = pending_.substr(0, len);
    event.raw = event.text;
    pending_.erase(0, len);
    return true;
}

struct InputModeGuard::TermiosStorage
{
    termios old_term{};
};

InputModeGuard::InputModeGuard()
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;

    termios_ = std::make_unique<TermiosStorage>();
    termios new_term{};
    if (tcgetattr(STDIN_FILENO, &termios_->old_term) == 0) {
        new_term = termios_->old_term;
        new_term.c_iflag &= ~static_cast<tcflag_t>(
            BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        new_term.c_cflag |= CS8;
        // Keep ISIG for terminals that still send legacy ETX. In structured
        // keyboard mode Ctrl-C is delivered as a key event and handled by the
        // runtime input loop.
        new_term.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN);
        new_term.c_cc[VMIN] = 0;
        new_term.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &new_term) == 0)
            termios_saved_ = true;
    }

    std::cout.write(
        kitty_push_structured_keyboard.data(),
        static_cast<std::streamsize>(kitty_push_structured_keyboard.size()));
    std::cout.flush();
    enabled_ = true;
}

InputModeGuard::~InputModeGuard()
{
    if (enabled_) {
        std::cout.write(
            kitty_pop_keyboard_mode.data(),
            static_cast<std::streamsize>(kitty_pop_keyboard_mode.size()));
        std::cout.flush();
    }

    if (termios_saved_)
        tcsetattr(STDIN_FILENO, TCSANOW, &termios_->old_term);
}

} // namespace nxt::input
