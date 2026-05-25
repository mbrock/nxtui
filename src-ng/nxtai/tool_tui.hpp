#pragma once

#include <nxt/any_layout.hpp>
#include <nxt/tui.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::tool_tui {

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

inline std::string primary_arg(const call_view & c)
{
    if (c.arguments.empty())
        return {};
    return truncate_bytes(c.arguments, 96);
}

inline auto chip(
    std::string s,
    Rgba8 fg_color,
    Rgba8 bg_color,
    Emphasis em_flags = DEFAULT_EMPHASIS)
{
    auto style = fg(fg_color) | bg(bg_color);
    if (em_flags != DEFAULT_EMPHASIS)
        style = style | em(em_flags);
    return text(std::move(s), style);
}

inline auto spine(const call_view & c)
{
    auto k = classify(c.name);
    switch (c.state) {
    case status::ok:
        return chip(" ok", slate_950, k.accent, Emphasis::bold);
    case status::error:
        return chip(" !!", slate_950, rose_300, Emphasis::bold);
    case status::running:
        return chip(" ..", amber_200, band_bg);
    }
    return chip("   ", slate_300, band_bg);
}

inline auto call_header(const call_view & c)
{
    auto k = classify(c.name);
    auto parts = std::vector<AnyLayout>{};
    parts.push_back(spine(c));
    parts.push_back(chip(
        std::format(" {} ", k.display), k.accent, band_bg, Emphasis::bold));
    parts.push_back(flex_text(primary_arg(c), fg(slate_500) | bg(band_bg)));
    if (c.elapsed_ms >= 0)
        parts.push_back(
            chip(std::format(" {}ms ", c.elapsed_ms), slate_500, band_bg));
    if (!c.output.empty())
        parts.push_back(chip(
            std::format(" {}B ", c.output.size()), slate_400, band_bg));
    return row(std::move(parts));
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

inline std::vector<std::string>
tail_lines(std::string_view text, std::size_t max_lines)
{
    auto lines = first_lines(text, static_cast<std::size_t>(-1));
    if (lines.size() > max_lines)
        lines.erase(lines.begin(), lines.begin() + (lines.size() - max_lines));
    return lines;
}

inline auto body_line(std::string s, Rgba8 fg_color)
{
    return text(std::move(s), fg(fg_color));
}

inline auto window_rows(std::vector<AnyLayout> rows, width_t pad = 2 * ch)
{
    auto padded = std::vector<AnyLayout>{};
    padded.reserve(rows.size());
    for (auto & r : rows) {
        padded.push_back(
            row(std::vector<AnyLayout>{
                hfill(pad, page_bg),
                std::move(r),
                flex_fill(page_bg),
            }));
    }
    return column(std::move(padded));
}

inline AnyLayout result_window(const call_view & c)
{
    auto rows = std::vector<AnyLayout>{};
    auto line_color = c.state == status::error ? rose_300 : slate_300;
    for (auto & line : first_lines(c.output, 4))
        rows.push_back(body_line(std::move(line), line_color));
    if (rows.empty())
        rows.push_back(body_line("running", slate_700));
    return window_rows(std::move(rows));
}

inline AnyLayout render_call(const call_view & c)
{
    auto pieces = std::vector<AnyLayout>{};
    pieces.push_back(call_header(c));
    if (!c.output.empty() || c.state == status::running)
        pieces.push_back(result_window(c));
    return column(std::move(pieces));
}

inline AnyLayout thought_block(std::string s)
{
    auto rows = std::vector<AnyLayout>{};
    rows.push_back(chip(" thinking ", slate_950, sky_300, Emphasis::bold));
    for (auto & line : tail_lines(s, 8))
        rows.push_back(body_line(std::move(line), sky_300));
    return window_rows(std::move(rows), 1 * ch);
}

inline AnyLayout render_turn(const turn_view & t)
{
    auto children = std::vector<AnyLayout>{};
    if (!t.thought.empty())
        children.push_back(thought_block(t.thought));
    for (const auto & c : t.calls)
        children.push_back(render_call(c));
    if (children.empty())
        children.push_back(text(""));
    return surface(
        Style{.fg = slate_300, .bg = page_bg, .em = DEFAULT_EMPHASIS},
        column(std::move(children)));
}

} // namespace nxt::ai::tool_tui
