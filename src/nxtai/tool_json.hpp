#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Small synchronous JSON readers for flat objects such as tool arguments.
// They are lenient about non-string values, which they skip without fully
// validating.
namespace nxtai::tools {

/// Advance `offset` past JSON whitespace.
inline void skip_json_ws(std::string_view input, std::size_t & offset)
{
    while (offset < input.size()) {
        auto ch = input[offset];
        if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t')
            return;
        ++offset;
    }
}

inline int json_hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

inline void append_json_utf8(std::string & out, std::uint32_t codepoint)
{
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

inline std::optional<std::uint16_t>
read_json_u16(std::string_view input, std::size_t & offset)
{
    if (offset + 4 > input.size())
        return std::nullopt;
    auto value = std::uint16_t{};
    for (auto i = 0; i < 4; ++i) {
        auto digit = json_hex_digit(input[offset++]);
        if (digit < 0)
            return std::nullopt;
        value = static_cast<std::uint16_t>((value << 4) | digit);
    }
    return value;
}

/// Read a JSON string literal at `offset` (no leading whitespace) and
/// return its decoded UTF-8 value, advancing `offset` past the closing
/// quote. Returns `std::nullopt` for a malformed string, a raw control
/// character, or an unpaired surrogate escape.
inline std::optional<std::string>
read_json_string(std::string_view input, std::size_t & offset)
{
    if (offset >= input.size() || input[offset++] != '"')
        return std::nullopt;

    auto out = std::string{};
    while (offset < input.size()) {
        auto ch = input[offset++];
        if (ch == '"')
            return out;
        if (static_cast<unsigned char>(ch) < 0x20)
            return std::nullopt;
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (offset >= input.size())
            return std::nullopt;
        auto escaped = input[offset++];
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
            auto first = read_json_u16(input, offset);
            if (!first)
                return std::nullopt;
            auto codepoint = static_cast<std::uint32_t>(*first);
            if (*first >= 0xd800 && *first <= 0xdbff) {
                if (offset + 2 > input.size() || input[offset++] != '\\'
                    || input[offset++] != 'u')
                    return std::nullopt;
                auto second = read_json_u16(input, offset);
                if (!second || *second < 0xdc00 || *second > 0xdfff)
                    return std::nullopt;
                codepoint = 0x10000
                            + (((static_cast<std::uint32_t>(*first) - 0xd800)
                                << 10)
                               | (static_cast<std::uint32_t>(*second)
                                  - 0xdc00));
            } else if (*first >= 0xdc00 && *first <= 0xdfff) {
                return std::nullopt;
            }
            append_json_utf8(out, codepoint);
            break;
        }
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

/// Skip one JSON value of any kind, stopping before the following `,` or
/// closing bracket. Strings are checked; other values are only scanned.
inline bool skip_json_sync_value(std::string_view input, std::size_t & offset)
{
    skip_json_ws(input, offset);
    if (offset >= input.size())
        return false;
    if (input[offset] == '"')
        return read_json_string(input, offset).has_value();

    auto stack = std::vector<char>{};
    do {
        auto ch = input[offset++];
        if (ch == '"') {
            --offset;
            if (!read_json_string(input, offset))
                return false;
            continue;
        }
        if (ch == '{')
            stack.push_back('}');
        else if (ch == '[')
            stack.push_back(']');
        else if (ch == '}' || ch == ']') {
            if (stack.empty() || stack.back() != ch) {
                --offset;
                return true;
            }
            stack.pop_back();
        } else if (stack.empty() && ch == ',') {
            --offset;
            return true;
        }
    } while (offset < input.size() && !stack.empty());

    while (offset < input.size() && input[offset] != ',' && input[offset] != '}')
        ++offset;
    return true;
}

/// The string-valued members of a JSON object, in order, as decoded
/// key/value pairs. Members with other value types are skipped. Returns
/// `std::nullopt` if `object` is not an object. Duplicate keys are kept and
/// anything after the closing brace is ignored.
[[nodiscard]] inline std::optional<
    std::vector<std::pair<std::string, std::string>>>
json_string_object(std::string_view object)
{
    auto offset = std::size_t{};
    skip_json_ws(object, offset);
    if (offset >= object.size() || object[offset++] != '{')
        return std::nullopt;

    auto out = std::vector<std::pair<std::string, std::string>>{};
    while (true) {
        skip_json_ws(object, offset);
        if (offset >= object.size())
            return std::nullopt;
        if (object[offset] == '}')
            return out;
        auto key = read_json_string(object, offset);
        if (!key)
            return std::nullopt;
        skip_json_ws(object, offset);
        if (offset >= object.size() || object[offset++] != ':')
            return std::nullopt;
        skip_json_ws(object, offset);
        if (offset < object.size() && object[offset] == '"') {
            auto value = read_json_string(object, offset);
            if (!value)
                return std::nullopt;
            out.emplace_back(std::move(*key), std::move(*value));
        } else if (!skip_json_sync_value(object, offset)) {
            return std::nullopt;
        }
        skip_json_ws(object, offset);
        if (offset >= object.size())
            return std::nullopt;
        if (object[offset] == '}')
            return out;
        if (object[offset++] != ',')
            return std::nullopt;
    }
}

/// Decoded value of the first string member named `key` in a JSON object,
/// or `std::nullopt` if the object is malformed, the key is absent, or its
/// value is not a string.
[[nodiscard]] inline std::optional<std::string>
json_string_member(std::string_view object, std::string_view key)
{
    auto members = json_string_object(object);
    if (!members)
        return std::nullopt;
    for (auto & [member_key, value] : *members)
        if (member_key == key)
            return std::move(value);
    return std::nullopt;
}

} // namespace nxtai::tools
