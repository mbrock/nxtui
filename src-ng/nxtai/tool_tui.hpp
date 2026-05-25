#pragma once

#include <nxt/rt/scoped_process.hpp>
#include <nxt/tui.hpp>
#include <nxt/tui_text.hpp>
#include <nxtai/openai_types.hpp>

#include <format>
#include <cstdint>
#include <optional>
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
    std::optional<nxt::rt::scoped_process::observation> observed;
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

inline auto status_chip(
    std::string_view s,
    Rgba8 fg_color,
    Rgba8 bg_color,
    Emphasis em_flags = DEFAULT_EMPHASIS)
{
    return chip(
        std::format(" {} ", s), fg_color, bg_color, em_flags);
}

template<Layout Body>
inline auto inset_block(Body && body, width_t pad = 1 * ch)
{
    return nxt::tui::row(
        hfill(pad, page_bg),
        grow_width(std::forward<Body>(body)));
}

template<Layout Header, Layout Body>
inline auto block(Header && header, Body && body, width_t pad = 1 * ch)
{
    return inset_block(
        column(std::forward<Header>(header), std::forward<Body>(body)),
        pad);
}

inline auto spine(const call_view & c)
{
    auto k = classify(c.name);
    switch (c.state) {
    case status::ok:
        return status_chip("ok", slate_950, k.accent, Emphasis::bold);
    case status::error:
        return status_chip("!!", slate_950, rose_300, Emphasis::bold);
    case status::running:
        return status_chip("..", amber_200, band_bg);
    }
    return status_chip("", slate_300, band_bg);
}

inline auto call_header(const call_view & c)
{
    auto k = classify(c.name);
    return row(
        spine(c),
        chip(
            std::format(" {} ", k.display),
            k.accent,
            band_bg,
            Emphasis::bold),
        flex_text(primary_arg(c), fg(slate_500) | bg(band_bg)),
        when(
            c.elapsed_ms >= 0,
            chip(std::format(" {}ms ", c.elapsed_ms), slate_500, band_bg)),
        when(
            !c.output.empty(),
            chip(std::format(" {}B ", c.output.size()), slate_400, band_bg)));
}

struct bash_arguments_view
{
    std::string command;
};

inline std::optional<std::string> bash_command(std::string_view arguments)
{
    if (arguments.empty())
        return std::nullopt;

    auto args = bash_arguments_view{};
    if (glz::read<openai::json_read_opts>(args, arguments))
        return std::nullopt;
    if (args.command.empty())
        return std::nullopt;
    return std::move(args.command);
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

inline auto body_line(std::string s, Rgba8 fg_color)
{
    return text(std::move(s), fg(fg_color));
}

inline auto body_lines(std::vector<std::string> lines, Rgba8 fg_color)
{
    if (lines.empty())
        lines.push_back({});

    auto styled = std::vector<std::vector<Span>>{};
    styled.reserve(lines.size());
    for (auto & line : lines)
        styled.push_back({span(std::move(line), fg(fg_color))});
    return styled_lines(
        std::move(styled), Style{.fg = fg_color, .bg = page_bg});
}

inline auto result_window(const call_view & c)
{
    auto line_color = c.state == status::error ? rose_300 : slate_300;
    auto lines = first_lines(c.output, 4);
    if (lines.empty())
        lines.push_back("running");
    return inset_block(body_lines(std::move(lines), line_color));
}

inline auto shell_header(
    const call_view & c,
    std::string title,
    Rgba8 title_color)
{
    auto latest_memory = std::string{};
    if (c.observed && c.observed->latest()) {
        const auto latest = *c.observed->latest();
        latest_memory = compact_bytes(latest.memory_current.v);
    }
    return row(
        flex_text(
            std::move(title),
            fg(title_color) | bg(band_bg) | em(Emphasis::bold)),
        when(
            c.elapsed_ms >= 0,
            chip(std::format(" {}ms ", c.elapsed_ms), slate_500, band_bg)),
        when(
            !latest_memory.empty(),
            chip(std::format(" {} ", latest_memory), slate_400, band_bg)),
        when(
            !c.output.empty(),
            chip(std::format(" {}B ", c.output.size()), slate_400, band_bg)));
}

inline auto shell_script_window(std::string_view command)
{
    auto rows = std::vector<std::vector<Span>>{};
    auto prefix = std::string{"$ "};
    for (auto & line : first_lines(command, 12)) {
        rows.push_back(
            {
                span(prefix, fg(orange_300)),
                span(std::move(line), fg(amber_200)),
            });
        prefix = "> ";
    }
    return inset_block(
        styled_lines(
            std::move(rows), Style{.fg = amber_200, .bg = page_bg}));
}

inline auto shell_output_window(const call_view & c)
{
    auto line_color = c.state == status::error ? rose_300 : slate_300;
    auto header =
        chip(
            c.state == status::running ? " running " : " output ",
            c.state == status::error ? rose_300 : slate_500,
            page_bg,
            c.state == status::running ? DEFAULT_EMPHASIS : Emphasis::bold);
    auto lines = std::vector<std::string>{};
    if (!c.output.empty()) {
        auto sanitized = nxt::tui::text_flow::sanitize_terminal_text(c.output);
        lines = nxt::tui::text_flow::wrap_text(sanitized, 88 * ch);
        if (lines.size() > 8)
            lines.resize(8);
    }
    if (c.output.empty() && c.state == status::running)
        lines.push_back("waiting for process output");
    return block(std::move(header), body_lines(std::move(lines), line_color));
}

inline auto render_bash_call(const call_view & c)
{
    auto command = bash_command(c.arguments);
    auto short_command = command && short_shell_oneliner(*command);
    auto title = short_command ? std::format("$ {}", *command)
                               : std::string{"shell script"};
    auto title_color = short_command ? amber_200 : orange_300;
    auto script = command.value_or(std::string{});
    return column(
        inset_block(shell_header(c, std::move(title), title_color)),
        when(
            command.has_value() && !short_command,
            shell_script_window(script)),
        when(
            !c.output.empty() || c.state == status::running,
            shell_output_window(c)));
}

inline auto render_generic_call(const call_view & c)
{
    return column(
        inset_block(call_header(c)),
        when(
            !c.output.empty() || c.state == status::running,
            result_window(c)));
}

inline auto render_call(const call_view & c)
{
    return either(c.name == "bash", render_generic_call(c), render_bash_call(c));
}

inline auto thought_block(std::string s)
{
    return block(
        chip(" thinking ", slate_950, sky_300, Emphasis::bold),
        nxt::tui::text_flow::markdown_block(
            s,
            fg(slate_300),
            88 * ch,
            Style{.fg = slate_300, .bg = page_bg}));
}

inline auto assistant_block(std::string s)
{
    return block(
        chip(" assistant ", slate_950, emerald_300, Emphasis::bold),
        nxt::tui::text_flow::markdown_block(
            s,
            fg(slate_300),
            88 * ch,
            Style{.fg = slate_300, .bg = page_bg}));
}

inline auto render_turn(const turn_view & t)
{
    auto has_thought = !t.thought.empty();
    auto has_calls = !t.calls.empty();
    return surface(
        Style{.fg = slate_300, .bg = page_bg, .em = DEFAULT_EMPHASIS},
        column(
            when(has_thought, thought_block(t.thought)),
            when(
                has_calls,
                each(
                    std::vector<call_view>{t.calls},
                    [](const call_view & c) {
                        return render_call(c);
                    })),
            when(!has_thought && !has_calls, text(""))));
}

} // namespace nxt::ai::tool_tui
