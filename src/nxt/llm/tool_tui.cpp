#include <nxt/llm/tool_tui.hpp>

#include <nxt/tui.hpp>
#include <nxt/tui_text.hpp>

#include <utility>

namespace nxt::llm::tool_tui {
namespace {

nxt::tui::Style chip_style(
    Rgba8 fg_color,
    Rgba8 bg_color,
    Emphasis em_flags)
{
    auto style = fg(fg_color) | bg(bg_color);
    if (em_flags != DEFAULT_EMPHASIS)
        style = style | em(em_flags);
    return style;
}

nxt::tui::AnyLayout row_layout(std::vector<nxt::tui::AnyLayout> children)
{
    return nxt::tui::row(std::move(children));
}

nxt::tui::AnyLayout column_layout(std::vector<nxt::tui::AnyLayout> children)
{
    return nxt::tui::column(std::move(children));
}

nxt::tui::AnyLayout spine(const call_view & c)
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

nxt::tui::AnyLayout call_header(const call_view & c)
{
    auto k = classify(c.name);
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(5);
    children.push_back(spine(c));
    children.push_back(chip(
        std::format(" {} ", k.display),
        k.accent,
        band_bg,
        Emphasis::bold));
    children.push_back(flex_text(primary_arg(c), fg(slate_500) | bg(band_bg)));
    if (c.elapsed_ms >= 0)
        children.push_back(
            chip(std::format(" {}ms ", c.elapsed_ms), slate_500, band_bg));
    if (!c.output.empty())
        children.push_back(
            chip(std::format(" {}B ", c.output.size()), slate_400, band_bg));
    return row_layout(std::move(children));
}

nxt::tui::AnyLayout result_window(const call_view & c)
{
    auto line_color = c.state == status::error ? rose_300 : slate_300;
    auto lines = first_lines(c.output, 4);
    if (lines.empty())
        lines.push_back("running");
    return inset_block(body_lines(std::move(lines), line_color));
}

nxt::tui::AnyLayout shell_header(
    const call_view & c,
    std::string title,
    Rgba8 title_color)
{
    auto latest_memory = std::string{};
    if (c.latest_memory_current)
        latest_memory = compact_bytes(*c.latest_memory_current);

    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(4);
    children.push_back(flex_text(
        std::move(title),
        fg(title_color) | bg(band_bg) | em(Emphasis::bold)));
    if (c.elapsed_ms >= 0)
        children.push_back(
            chip(std::format(" {}ms ", c.elapsed_ms), slate_500, band_bg));
    if (!latest_memory.empty())
        children.push_back(chip(
            std::format(" {} ", latest_memory), slate_400, band_bg));
    if (!c.output.empty())
        children.push_back(
            chip(std::format(" {}B ", c.output.size()), slate_400, band_bg));
    return row_layout(std::move(children));
}

nxt::tui::AnyLayout shell_script_window(std::string_view command)
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

nxt::tui::AnyLayout shell_output_window(const call_view & c)
{
    auto line_color = c.state == status::error ? rose_300 : slate_300;
    auto header = chip(
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

nxt::tui::AnyLayout render_bash_call(const call_view & c)
{
    auto command = bash_command(c.arguments);
    auto short_command = command && short_shell_oneliner(*command);
    auto title = short_command ? std::format("$ {}", *command)
                 : c.arguments.empty() ? std::string{"shell script"}
                                       : truncate_bytes(c.arguments, 96);
    auto title_color = short_command ? amber_200 : orange_300;
    auto script = command.value_or(std::string{});

    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(3);
    children.push_back(inset_block(shell_header(c, std::move(title), title_color)));
    if (command.has_value() && !short_command)
        children.push_back(shell_script_window(script));
    if (!c.output.empty() || c.state == status::running)
        children.push_back(shell_output_window(c));
    return column_layout(std::move(children));
}

nxt::tui::AnyLayout render_generic_call(const call_view & c)
{
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(2);
    children.push_back(inset_block(call_header(c)));
    if (!c.output.empty() || c.state == status::running)
        children.push_back(result_window(c));
    return column_layout(std::move(children));
}

} // namespace

nxt::tui::AnyLayout chip(
    std::string s,
    Rgba8 fg_color,
    Rgba8 bg_color,
    Emphasis em_flags)
{
    return text(std::move(s), chip_style(fg_color, bg_color, em_flags));
}

nxt::tui::AnyLayout status_chip(
    std::string_view s,
    Rgba8 fg_color,
    Rgba8 bg_color,
    Emphasis em_flags)
{
    return chip(std::format(" {} ", s), fg_color, bg_color, em_flags);
}

nxt::tui::AnyLayout inset_block(nxt::tui::AnyLayout body, width_t pad)
{
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(2);
    children.push_back(hfill(pad, page_bg));
    children.push_back(grow_width(std::move(body)));
    return row_layout(std::move(children));
}

nxt::tui::AnyLayout block(
    nxt::tui::AnyLayout header,
    nxt::tui::AnyLayout body,
    width_t pad)
{
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(2);
    children.push_back(std::move(header));
    children.push_back(std::move(body));
    return inset_block(column_layout(std::move(children)), pad);
}

nxt::tui::AnyLayout body_line(std::string s, Rgba8 fg_color)
{
    return text(std::move(s), fg(fg_color));
}

nxt::tui::AnyLayout body_lines(std::vector<std::string> lines, Rgba8 fg_color)
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

nxt::tui::AnyLayout render_call(const call_view & c)
{
    if (c.name == "bash")
        return render_bash_call(c);
    return render_generic_call(c);
}

nxt::tui::AnyLayout thought_block(std::string s)
{
    return block(
        chip(" thinking ", slate_950, sky_300, Emphasis::bold),
        nxt::tui::text_flow::markdown_block(
            s,
            fg(slate_300),
            88 * ch,
            Style{.fg = slate_300, .bg = page_bg}));
}

nxt::tui::AnyLayout assistant_block(std::string s)
{
    return block(
        chip(" assistant ", slate_950, emerald_300, Emphasis::bold),
        nxt::tui::text_flow::markdown_block(
            s,
            fg(slate_300),
            88 * ch,
            Style{.fg = slate_300, .bg = page_bg}));
}

nxt::tui::AnyLayout render_turn(const turn_view & t)
{
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(3);
    if (!t.thought.empty())
        children.push_back(thought_block(t.thought));
    if (!t.calls.empty()) {
        children.push_back(each(
            std::vector<call_view>{t.calls},
            [](const call_view & c) {
                return render_call(c);
            }));
    }
    if (t.thought.empty() && t.calls.empty())
        children.push_back(text(""));

    return surface(
        Style{.fg = slate_300, .bg = page_bg, .em = DEFAULT_EMPHASIS},
        column_layout(std::move(children)));
}

} // namespace nxt::llm::tool_tui
