#pragma once

#include <nxt/any_layout.hpp>
#include <nxt/style.hpp>
#include <nxt/units.hpp>
#include <nxt/llm/tool_json.hpp>

#include <format>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::llm::tool_tui {

using namespace nxt::tui;

constexpr Rgba8 slate_950{2, 6, 23};
constexpr Rgba8 slate_900{15, 23, 42};
constexpr Rgba8 slate_800{30, 41, 59};
constexpr Rgba8 slate_700{51, 65, 85};
constexpr Rgba8 slate_500{100, 116, 139};
constexpr Rgba8 slate_400{148, 163, 184};
constexpr Rgba8 slate_300{203, 213, 225};
constexpr Rgba8 amber_200{253, 230, 138};
constexpr Rgba8 amber_300{252, 211, 77};
constexpr Rgba8 emerald_300{110, 231, 183};
constexpr Rgba8 orange_300{253, 186, 116};
constexpr Rgba8 violet_300{196, 181, 253};
constexpr Rgba8 lime_300{190, 242, 100};
constexpr Rgba8 teal_300{94, 234, 212};
constexpr Rgba8 sky_300{125, 211, 252};
constexpr Rgba8 rose_300{253, 164, 175};

constexpr Rgba8 page_bg = slate_950;
constexpr Rgba8 band_bg = slate_900;

enum class status { running, ok, error };

struct tool_kind
{
    std::string_view display;
    Rgba8 accent;
};

struct call_view
{
    std::string name;
    std::string arguments;
    std::string output;
    std::optional<std::uint64_t> latest_memory_current;
    status state = status::running;
    int elapsed_ms = -1;
};

struct turn_view
{
    std::string thought;
    std::vector<call_view> calls;
};

inline tool_kind classify(std::string_view name)
{
    using namespace std::literals;
    if (name == "rg_search"sv)
        return {"find", amber_300};
    if (name == "read_file"sv)
        return {"file", emerald_300};
    if (name == "bash"sv)
        return {"bash", orange_300};
    if (name == "web_fetch"sv)
        return {"fetch", violet_300};
    if (name.starts_with("nxt_"sv))
        return {name.substr(4), lime_300};
    return {name, teal_300};
}

inline std::string truncate_bytes(std::string s, std::size_t max)
{
    if (s.size() <= max)
        return s;
    if (max <= 3)
        return s.substr(0, max);
    s.resize(max - 3);
    s += "...";
    return s;
}

inline std::string compact_bytes(std::uint64_t bytes)
{
    if (bytes < 1000)
        return std::format("{}B", bytes);
    auto kib = static_cast<double>(bytes) / 1024.0;
    if (kib < 1000.0)
        return std::format("{:.0f}K", kib);
    auto mib = kib / 1024.0;
    if (mib < 1000.0)
        return std::format("{:.1f}M", mib);
    return std::format("{:.1f}G", mib / 1024.0);
}

inline std::string primary_arg(const call_view & c)
{
    if (c.arguments.empty())
        return {};
    return truncate_bytes(c.arguments, 96);
}

inline std::optional<std::string> bash_command(std::string_view arguments)
{
    if (arguments.empty())
        return std::nullopt;

    auto command = nxt::llm::tools::json_string_member(arguments, "command");
    if (!command || command->empty())
        return std::nullopt;
    return command;
}

inline bool short_shell_oneliner(std::string_view command)
{
    return command.find('\n') == std::string_view::npos
        && command.find('\r') == std::string_view::npos
        && command.size() <= 86;
}

inline std::vector<std::string>
first_lines(std::string_view text, std::size_t max_lines)
{
    auto lines = std::vector<std::string>{};
    while (!text.empty() && lines.size() < max_lines) {
        auto end = text.find('\n');
        auto line =
            end == std::string_view::npos ? text : text.substr(0, end);
        lines.push_back(truncate_bytes(std::string{line}, 180));
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return lines;
}

nxt::tui::AnyLayout chip(
    std::string s,
    Rgba8 fg_color,
    Rgba8 bg_color,
    Emphasis em_flags = DEFAULT_EMPHASIS);

nxt::tui::AnyLayout status_chip(
    std::string_view s,
    Rgba8 fg_color,
    Rgba8 bg_color,
    Emphasis em_flags = DEFAULT_EMPHASIS);

nxt::tui::AnyLayout inset_block(
    nxt::tui::AnyLayout body,
    width_t pad = 1 * ch);

nxt::tui::AnyLayout block(
    nxt::tui::AnyLayout header,
    nxt::tui::AnyLayout body,
    width_t pad = 1 * ch);

nxt::tui::AnyLayout body_line(std::string s, Rgba8 fg_color);
nxt::tui::AnyLayout body_lines(
    std::vector<std::string> lines,
    Rgba8 fg_color);
nxt::tui::AnyLayout render_call(const call_view & c);
nxt::tui::AnyLayout thought_block(std::string s);
nxt::tui::AnyLayout assistant_block(std::string s);
nxt::tui::AnyLayout render_turn(const turn_view & t);

} // namespace nxt::llm::tool_tui
