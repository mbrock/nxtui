#pragma once

#include "nxtui/tui.hpp"
#include "nxtui/utf8.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace nxtui::tui::text_flow {

inline void trim_trailing_space(std::string & text)
{
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
}

inline std::size_t markdown_list_continuation_indent(std::string_view text)
{
    if (text.starts_with("- ") || text.starts_with("* "))
        return 2;
    auto i = std::size_t{0};
    while (i < text.size() && text[i] >= '0' && text[i] <= '9')
        ++i;
    if (i > 0 && i + 1 < text.size() && text[i] == '.'
        && text[i + 1] == ' ')
        return i + 2;
    return 0;
}

inline std::vector<std::string>
wrap_text(std::string_view text, width_t wrap_width)
{
    auto lines = std::vector<std::string>{};
    auto current = std::string{};
    auto cursor = 0 * ch;
    wrap_width = std::max(wrap_width, 1 * ch);

    auto finish_line = [&] {
        trim_trailing_space(current);
        lines.push_back(std::move(current));
        current = {};
        cursor = 0 * ch;
    };

    auto saw_paragraph = false;
    for (auto paragraph : utf8::paragraphs(text)) {
        if (saw_paragraph) {
            finish_line();
            lines.emplace_back();
        }

        auto continuation_indent =
            markdown_list_continuation_indent(paragraph.text);
        auto emitted_paragraph_lines = std::size_t{0};
        auto start_continuation = [&] {
            if (continuation_indent == 0 || emitted_paragraph_lines == 0)
                return;
            current = std::string(continuation_indent, ' ');
            cursor = continuation_indent * ch;
        };
        auto finish_paragraph_line = [&] {
            finish_line();
            ++emitted_paragraph_lines;
            start_continuation();
        };

        for (auto segment : utf8::segments(paragraph.text)) {
            if (segment.kind == utf8::text_segment::kind_t::line_break) {
                finish_paragraph_line();
                continue;
            }
            if (cursor > 0 * ch && cursor + segment.width > wrap_width)
                finish_paragraph_line();
            current += segment.text;
            current += ' ';
            cursor += segment.width + 1 * ch;
        }

        saw_paragraph = true;
    }

    if (!current.empty() || lines.empty())
        finish_line();
    return lines;
}

inline std::vector<Span>
parse_inline_markdown(std::string_view text, Style base_style)
{
    auto spans = std::vector<Span>{};

    auto push_plain = [&](std::string_view part) {
        if (!part.empty())
            spans.push_back(span(std::string{part}, base_style));
    };

    auto pos = std::size_t{0};
    while (pos < text.size()) {
        auto bold_pos = text.find("**", pos);
        auto code_pos = text.find('`', pos);
        auto next = std::min(
            bold_pos == std::string_view::npos ? text.size() : bold_pos,
            code_pos == std::string_view::npos ? text.size() : code_pos);

        push_plain(text.substr(pos, next - pos));
        pos = next;
        if (pos >= text.size())
            break;

        if (bold_pos == pos) {
            auto close = text.find("**", pos + 2);
            if (close != std::string_view::npos) {
                auto inner = text.substr(pos + 2, close - pos - 2);
                if (!inner.empty())
                    spans.push_back(
                        span(std::string{inner}, base_style | bold));
                pos = close + 2;
                continue;
            }
            push_plain("**");
            pos += 2;
            continue;
        }

        auto close = text.find('`', pos + 1);
        if (close != std::string_view::npos) {
            auto inner = text.substr(pos + 1, close - pos - 1);
            if (!inner.empty())
                spans.push_back(
                    span(
                        std::string{inner},
                        base_style | fg(Rgba8{230, 215, 150})
                            | bg(Rgba8{42, 45, 48})));
            pos = close + 1;
            continue;
        }

        push_plain("`");
        ++pos;
    }

    return spans;
}

inline std::vector<std::vector<Span>> markdown_lines(
    std::string_view text,
    Style base_style,
    width_t wrap_width)
{
    auto lines = std::vector<std::vector<Span>>{};

    auto push_blank_once = [&] {
        if (lines.empty() || !lines.back().empty())
            lines.push_back({});
    };

    auto saw_paragraph = false;
    for (auto paragraph : utf8::paragraphs(text)) {
        if (saw_paragraph)
            push_blank_once();

        auto content = paragraph.text;
        auto paragraph_style = base_style;
        auto padded = false;
        if (content.size() >= 4 && content.starts_with("**")
            && content.ends_with("**")) {
            content.remove_prefix(2);
            content.remove_suffix(2);
            paragraph_style = paragraph_style | bold;
            padded = true;
        }

        if (padded)
            push_blank_once();

        for (const auto & line : wrap_text(content, wrap_width))
            lines.push_back(parse_inline_markdown(line, paragraph_style));

        if (padded)
            push_blank_once();

        saw_paragraph = true;
    }

    if (lines.empty())
        lines.push_back({});
    return lines;
}

inline auto markdown_block(
    std::string_view text,
    Style base_style,
    width_t wrap_width,
    Style clear = {})
{
    return styled_lines(
        markdown_lines(text, base_style, wrap_width), clear);
}

inline std::string sanitize_terminal_text(std::string_view text)
{
    auto out = std::string{};
    out.reserve(text.size());
    auto i = std::size_t{0};
    while (i < text.size()) {
        auto c = static_cast<unsigned char>(text[i]);
        if (c == 0x1b) {
            ++i;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size()) {
                    auto x = static_cast<unsigned char>(text[i]);
                    ++i;
                    if (x >= 0x40 && x <= 0x7e)
                        break;
                }
            } else {
                ++i;
            }
            continue;
        }
        if (c == '\t') {
            out += "    ";
            ++i;
            continue;
        }
        if (c == '\n') {
            out.push_back('\n');
            ++i;
            continue;
        }
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                out.push_back('\n');
                i += 2;
                continue;
            }
            ++i;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            ++i;
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
}

} // namespace nxtui::tui::text_flow
