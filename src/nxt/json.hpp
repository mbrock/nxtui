#pragma once

#include "nxt/rt/task.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace nxt::json {

struct parse_error : rt::runtime_error
{
    using rt::runtime_error::runtime_error;
};

struct string_reader
{
    std::string_view input;
    std::size_t offset = 0;

    rt::task<std::optional<char>> peek()
    {
        if (offset >= input.size())
            co_return std::nullopt;
        co_return input[offset];
    }

    rt::task<std::optional<char>> take()
    {
        if (offset >= input.size())
            co_return std::nullopt;
        co_return input[offset++];
    }
};

enum class token_kind
{
    object_begin,
    object_end,
    array_begin,
    array_end,
    colon,
    comma,
    string,
    number,
    boolean,
    null,
};

struct token
{
    token_kind kind;
    std::string text;
    bool boolean = false;
};

struct writer
{
    std::string out;

    void raw(std::string_view text)
    {
        out += text;
    }

    void character(char c)
    {
        out.push_back(c);
    }

    void string(std::string_view text)
    {
        out.push_back('"');
        for (auto c : text) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    auto v = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out.push_back(hex[v >> 4]);
                    out.push_back(hex[v & 0xf]);
                } else {
                    out.push_back(c);
                }
                break;
            }
        }
        out.push_back('"');
    }

    void key(std::string_view name)
    {
        string(name);
        out.push_back(':');
    }

    void boolean(bool value)
    {
        out += value ? "true" : "false";
    }

    void number(std::size_t value)
    {
        out += std::to_string(value);
    }
};

inline token make_token(token_kind kind)
{
    return token{.kind = kind, .text = {}, .boolean = false};
}

inline token make_text_token(token_kind kind, std::string text)
{
    return token{.kind = kind, .text = std::move(text), .boolean = false};
}

inline token make_bool_token(bool value)
{
    return token{.kind = token_kind::boolean, .text = {}, .boolean = value};
}

inline void append_utf8(std::string & out, std::uint32_t cp)
{
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
}

inline std::optional<unsigned> hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<unsigned>(c - 'A' + 10);
    return std::nullopt;
}

template<typename Reader>
rt::task<void> skip_ws(Reader & in)
{
    while (auto ch = co_await in.peek()) {
        if (*ch != ' ' && *ch != '\n' && *ch != '\r' && *ch != '\t')
            co_return;
        co_await in.take();
    }
}

template<typename Reader>
rt::task<char> require_char(Reader & in, std::string_view context)
{
    auto ch = co_await in.take();
    if (!ch)
        throw parse_error{"unexpected end of JSON while reading " + std::string{context}};
    co_return *ch;
}

template<typename Reader>
rt::task<std::uint32_t> read_u16_escape(Reader & in)
{
    auto value = std::uint32_t{};
    for (auto i = 0; i < 4; ++i) {
        auto ch = co_await require_char(in, "unicode escape");
        auto hex = hex_value(ch);
        if (!hex)
            throw parse_error{"invalid JSON unicode escape"};
        value = (value << 4) | *hex;
    }
    co_return value;
}

template<typename Reader>
rt::task<std::string> read_string_body(Reader & in)
{
    auto out = std::string{};
    while (true) {
        auto ch = co_await require_char(in, "string");
        if (ch == '"')
            co_return out;
        if (static_cast<unsigned char>(ch) < 0x20)
            throw parse_error{"control character in JSON string"};
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }

        auto escaped = co_await require_char(in, "string escape");
        switch (escaped) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            auto cp = co_await read_u16_escape(in);
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (co_await require_char(in, "low surrogate escape") != '\\'
                    || co_await require_char(in, "low surrogate escape") != 'u')
                    throw parse_error{"missing JSON low surrogate"};
                auto low = co_await read_u16_escape(in);
                if (low < 0xdc00 || low > 0xdfff)
                    throw parse_error{"invalid JSON low surrogate"};
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                throw parse_error{"unpaired JSON low surrogate"};
            }
            append_utf8(out, cp);
            break;
        }
        default:
            throw parse_error{"invalid JSON string escape"};
        }
    }
}

template<typename Reader>
rt::task<std::string> read_string(Reader & in)
{
    co_await skip_ws(in);
    if (co_await require_char(in, "string") != '"')
        throw parse_error{"expected JSON string"};
    co_return co_await read_string_body(in);
}

template<typename Reader>
rt::task<std::string> read_number_text(Reader & in, char first)
{
    auto out = std::string{first};
    while (auto ch = co_await in.peek()) {
        auto c = *ch;
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E'
              || c == '+' || c == '-'))
            break;
        out.push_back(c);
        co_await in.take();
    }
    co_return out;
}

template<typename Reader>
rt::task<bool> read_literal_tail(Reader & in, std::string_view tail)
{
    for (auto expected : tail) {
        auto ch = co_await in.take();
        if (!ch || *ch != expected)
            co_return false;
    }
    co_return true;
}

template<typename Reader>
rt::task<std::optional<token>> read_token(Reader & in)
{
    co_await skip_ws(in);
    auto ch = co_await in.take();
    if (!ch)
        co_return std::nullopt;

    switch (*ch) {
    case '{': co_return make_token(token_kind::object_begin);
    case '}': co_return make_token(token_kind::object_end);
    case '[': co_return make_token(token_kind::array_begin);
    case ']': co_return make_token(token_kind::array_end);
    case ':': co_return make_token(token_kind::colon);
    case ',': co_return make_token(token_kind::comma);
    case '"':
        co_return make_text_token(token_kind::string, co_await read_string_body(in));
    case 't':
        if (co_await read_literal_tail(in, "rue"))
            co_return make_bool_token(true);
        break;
    case 'f':
        if (co_await read_literal_tail(in, "alse"))
            co_return make_bool_token(false);
        break;
    case 'n':
        if (co_await read_literal_tail(in, "ull"))
            co_return make_token(token_kind::null);
        break;
    default:
        if ((*ch >= '0' && *ch <= '9') || *ch == '-')
            co_return make_text_token(
                token_kind::number, co_await read_number_text(in, *ch));
        break;
    }

    throw parse_error{"invalid JSON token"};
}

} // namespace nxt::json
