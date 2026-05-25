#pragma once

#include <nxt/any_layout.hpp>
#include <nxt/tui.hpp>
#include <nxt/tui_text.hpp>
#include <nxtai/openai_types.hpp>

#include <format>
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

inline auto block(std::vector<AnyLayout> rows, width_t pad = 1 * ch)
{
    return inset_block(dyn_column(std::move(rows)), pad);
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

inline AnyLayout shell_header(
    const call_view & c,
    std::string title,
    Rgba8 title_color)
{
    auto parts = std::vector<AnyLayout>{};
    parts.push_back(flex_text(std::move(title),
                              fg(title_color) | bg(band_bg)
                                  | em(Emphasis::bold)));
    if (c.elapsed_ms >= 0)
        parts.push_back(
            chip(std::format(" {}ms ", c.elapsed_ms), slate_500, band_bg));
    if (!c.output.empty())
        parts.push_back(chip(
            std::format(" {}B ", c.output.size()), slate_400, band_bg));
    return row(std::move(parts));
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

inline AnyLayout render_bash_call(const call_view & c)
{
    auto command = bash_command(c.arguments);
    auto pieces = std::vector<AnyLayout>{};
    if (command && short_shell_oneliner(*command)) {
        pieces.push_back(inset_block(
            shell_header(c, std::format("$ {}", *command), amber_200)));
    } else {
        pieces.push_back(inset_block(
            shell_header(c, "shell script", orange_300)));
        if (command)
            pieces.push_back(shell_script_window(*command));
    }

    if (!c.output.empty() || c.state == status::running)
        pieces.push_back(shell_output_window(c));
    return column(std::move(pieces));
}

inline AnyLayout render_call(const call_view & c)
{
    if (c.name == "bash")
        return render_bash_call(c);

    auto pieces = std::vector<AnyLayout>{};
    pieces.push_back(inset_block(call_header(c)));
    if (!c.output.empty() || c.state == status::running)
        pieces.push_back(result_window(c));
    return column(std::move(pieces));
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
