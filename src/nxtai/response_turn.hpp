#pragma once

#include <glaze/glaze_exceptions.hpp>

#include "nxt/tui.hpp"
#include "nxt/utf8.hpp"
#include <nxtai/agent.hpp>
#include <nxtai/agent_trace.hpp>
#include <nxtai/hud_blocks.hpp>
#include <nxtai/responses.hpp>
#include <nxtai/tools.hpp>
#include <nxtio/arrow.hpp>
#include <nxtio/async.hpp>
#include <nxtio/buffers.hpp>
#include <nxtio/net.hpp>
#include <nxtio/process.hpp>
#include <nxtio/stacktrace.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::response_turn {

using llm_request = responses::openai_responses_request;
using response_stream_result = agent::response_stream_result;
using stream_event = responses::stream_event;

inline nxt::io::net::endpoint openai_responses_endpoint()
{
    return nxt::io::net::endpoint{
        .host = "api.openai.com",
        .port = 443,
    };
}

inline std::size_t stream_wrap_width(nxt::ui::yard & self)
{
    auto columns = self.runtime().terminal_width().count();
    if (columns > 32)
        return columns - 8;
    return std::max<std::size_t>(1, columns);
}

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
wrap_stream_text(std::string_view text, std::size_t wrap_width)
{
    auto lines = std::vector<std::string>{};
    auto current = std::string{};
    auto cursor = std::size_t{0};
    wrap_width = std::max<std::size_t>(1, wrap_width);

    auto finish_line = [&] {
        trim_trailing_space(current);
        lines.push_back(std::move(current));
        current = {};
        cursor = 0;
    };

    auto saw_paragraph = false;
    for (auto paragraph : nxt::utf8::paragraphs(text)) {
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
            cursor = continuation_indent;
        };
        auto finish_paragraph_line = [&] {
            finish_line();
            ++emitted_paragraph_lines;
            start_continuation();
        };
        for (auto segment : nxt::utf8::segments(paragraph.text))
            if (segment.kind == nxt::utf8::text_segment::kind_t::line_break) {
                finish_paragraph_line();
            } else {
                auto word_width = segment.width.count();
                if (cursor > 0 && cursor + word_width > wrap_width)
                    finish_paragraph_line();
                current += segment.text;
                current += ' ';
                cursor += word_width + 1;
            }

        saw_paragraph = true;
    }

    if (!current.empty() || lines.empty())
        finish_line();
    return lines;
}

inline std::string
wrapped_stream_text(std::string_view text, std::size_t wrap_width)
{
    auto lines = wrap_stream_text(text, wrap_width);
    auto out = std::string{};
    for (const auto & line : lines) {
        if (!out.empty())
            out += '\n';
        out += line;
    }
    return out;
}

inline std::vector<tui::Span>
parse_inline_markdown(std::string_view text, tui::Style base_style)
{
    auto spans = std::vector<tui::Span>{};

    auto push_plain = [&](std::string_view part) {
        if (!part.empty())
            spans.push_back(tui::span(std::string{part}, base_style));
    };

    auto pos = std::size_t{0};
    while (pos < text.size()) {
        auto bold = text.find("**", pos);
        auto code = text.find('`', pos);
        auto next = std::min(
            bold == std::string_view::npos ? text.size() : bold,
            code == std::string_view::npos ? text.size() : code);

        push_plain(text.substr(pos, next - pos));
        pos = next;
        if (pos >= text.size())
            break;

        if (bold == pos) {
            auto close = text.find("**", pos + 2);
            if (close != std::string_view::npos) {
                auto inner = text.substr(pos + 2, close - pos - 2);
                if (!inner.empty())
                    spans.push_back(
                        tui::span(std::string{inner}, base_style | tui::bold));
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
                spans.push_back(tui::span(
                    std::string{inner},
                    base_style | tui::fg(nxt::Rgba8{230, 215, 150})
                        | tui::bg(nxt::Rgba8{42, 45, 48})));
            pos = close + 1;
            continue;
        }

        push_plain("`");
        ++pos;
    }

    return spans;
}

inline auto markdown_text_block(
    std::string_view text,
    tui::Style base_style,
    std::size_t wrap_width)
{
    auto lines = std::vector<std::vector<tui::Span>>{};

    auto push_blank_once = [&] {
        if (lines.empty() || !lines.back().empty())
            lines.push_back({});
    };

    auto saw_paragraph = false;
    for (auto paragraph : nxt::utf8::paragraphs(text)) {
        if (saw_paragraph)
            push_blank_once();

        auto content = paragraph.text;
        auto paragraph_style = base_style;
        auto padded = false;
        if (content.size() >= 4 && content.starts_with("**")
            && content.ends_with("**")) {
            content.remove_prefix(2);
            content.remove_suffix(2);
            paragraph_style = paragraph_style | tui::bold;
            padded = true;
        }

        if (padded)
            push_blank_once();

        for (const auto & line : wrap_stream_text(content, wrap_width))
            lines.push_back(parse_inline_markdown(line, paragraph_style));

        if (padded)
            push_blank_once();

        saw_paragraph = true;
    }

    return tui::styled_lines(std::move(lines), base_style);
}

inline auto folded_thought_block()
{
    return hud_blocks::header_row(
        "▸",
        "thought",
        "folded",
        "",
        nxt::Rgba8::cyan(),
        tui::fg(nxt::Rgba8{120, 150, 165}) | tui::faint);
}

inline auto thought_marquee_block(std::string_view text)
{
    auto one_line = std::string{};
    one_line.reserve(text.size());
    for (auto ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t')
            one_line += ' ';
        else
            one_line += ch;
    }
    trim_trailing_space(one_line);
    return hud_blocks::header_row(
        "≈",
        "thinking",
        hud_blocks::truncate_cells(one_line, 90 * ch),
        "",
        nxt::Rgba8::cyan(),
        tui::fg(nxt::Rgba8{145, 185, 195}) | tui::faint);
}

inline auto status_row(std::string detail)
{
    return hud_blocks::header_row(
        "◆",
        "agent",
        std::move(detail),
        "",
        nxt::Rgba8{150, 190, 255},
        tui::fg(nxt::Rgba8{180, 190, 210}) | tui::faint);
}

inline auto pending_stream_block(
    std::string text, tui::Style style, std::size_t wrap_width)
{
    auto lines = wrap_stream_text(text, wrap_width);
    constexpr auto max_lines = std::size_t{6};
    if (lines.size() > max_lines)
        lines.erase(lines.begin(), lines.end() - max_lines);

    auto height = std::max<std::size_t>(1, lines.size()) * ln;
    auto width = 1 * ch;
    for (const auto & line : lines)
        width = std::max(width, tui::utf8_width(line));

    return tui::leaf(
        tui::WidthHint{width, 1.0 * one},
        tui::HeightHint::fixed(height),
        [lines = std::move(lines), style](RasterView & r, Size size) {
            std::ranges::fill(r.glyphs(), 32);
            std::ranges::fill(r.fgs(), DEFAULT_COLOR);
            std::ranges::fill(r.bgs(), DEFAULT_COLOR);
            std::ranges::fill(r.ems(), DEFAULT_EMPHASIS);

            auto row = 0 * ln;
            for (const auto & line : lines) {
                if (row >= size.h)
                    break;
                tui::render_span(
                    r, Pos::at(0 * ch, row), tui::span(line, style));
                row += 1 * ln;
            }
        });
}

inline auto output_separator()
{
    return tui::fixed_height(1 * ln, tui::hrule());
}

inline void for_complete_words(std::string & text, bool finish, auto fn)
{
    auto n =
        finish ? text.size() : nxt::utf8::complete_words_prefix_size(text);
    auto complete = std::string_view{text}.substr(0, n);
    for (auto segment : nxt::utf8::segments(complete))
        fn(segment);
    text.erase(0, n);
}

template<typename Stream>
auto response_event_source(nxt::ui::yard & self, Stream & stream)
{
    return nxt::make_source<stream_event>(
        [&self, &stream](
            std::stop_token) -> nxt::task<std::optional<stream_event>> {
            auto event = co_await stream.next();
            if (event)
                agent_trace::record_llm_event(self, *event);
            co_return event;
        });
}

inline bool is_response_lifecycle_event(const stream_event & event)
{
    return event.type == "response.created"
           || event.type == "response.in_progress"
           || event.type == "response.completed"
           || event.type == "response.failed"
           || event.type == "response.incomplete";
}

inline std::string event_detail(const stream_event & event)
{
    auto data = event.data;
    if (data.size() > 240)
        data = data.substr(0, 237) + "...";
    return std::format("{}: {}", event.type, data);
}

[[noreturn]] inline void throw_unexpected_event(
    std::string_view context,
    const stream_event & event)
{
    throw nxt::io::runtime_error{
        std::format(
            "unexpected OpenAI stream event while {}: {}",
            context,
            event_detail(event))};
}

[[noreturn]] inline void throw_unexpected_eof(std::string_view context)
{
    throw nxt::io::runtime_error{
        std::format(
            "OpenAI response stream ended while {}; expected a terminal event",
            context)};
}

inline bool is_text_item_scaffolding_event(const stream_event & event)
{
    return event.type == "response.content_part.added"
           || event.type == "response.content_part.done"
           || event.type == "response.output_text.done"
           || event.type == "response.reasoning_summary_part.added"
           || event.type == "response.reasoning_summary_part.done"
           || event.type == "response.reasoning_summary_text.done";
}

inline bool is_function_call_item_event(const stream_event & event)
{
    return event.type == "response.function_call_arguments.delta"
           || event.type == "response.function_call_arguments.done";
}

inline std::exception_ptr classify_response_failure(
    nxt::ui::UIRuntime & runtime,
    std::exception_ptr failure)
{
    try {
        nxt::io::rethrow(failure);
    } catch (const nxt::io::operation_cancelled &) {
        if (!runtime.shutdown_requested())
            return failure;
    } catch (const nxt::cancelled &) {
        if (!runtime.shutdown_requested())
            return failure;
    } catch (...) {
        return failure;
    }
    return {};
}

[[noreturn]] inline void throw_stream_lifecycle_failure(
    std::string_view work_context,
    std::exception_ptr work_failure,
    std::string_view shutdown_context,
    std::exception_ptr shutdown_failure)
{
    if (work_failure && shutdown_failure)
        throw nxt::exception_group{
            std::format("{}; {}", work_context, shutdown_context),
            {work_failure, shutdown_failure}};
    if (work_failure)
        nxt::io::rethrow(work_failure);
    nxt::io::rethrow(shutdown_failure);
}

template<typename Result, typename Work, typename Shutdown>
nxt::task<Result> run_then_shutdown(
    nxt::ui::UIRuntime & runtime,
    std::string_view work_context,
    Work work,
    std::string_view shutdown_context,
    Shutdown shutdown)
{
    auto result = Result{};
    auto work_failure = std::exception_ptr{};
    auto shutdown_failure = std::exception_ptr{};

    try {
        result = co_await nxt::invoke(std::move(work));
    } catch (...) {
        work_failure = classify_response_failure(
            runtime, std::current_exception());
    }

    try {
        co_await nxt::invoke(std::move(shutdown));
    } catch (...) {
        shutdown_failure = std::current_exception();
    }

    if (work_failure || shutdown_failure)
        throw_stream_lifecycle_failure(
            work_context,
            work_failure,
            shutdown_context,
            shutdown_failure);

    co_return result;
}

template<typename Stream>
nxt::task<std::optional<openai::raw_json>> read_text_delta_item(
    Stream & stream,
    nxt::ui::yard & self,
    std::string_view delta_event_type,
    tui::Style style,
    bool fold_when_done,
    bool separate_before_commit,
    hud_blocks::State * hud)
{
    auto text = std::string{};
    auto wrap_width = stream_wrap_width(self);
    auto block = std::string{};
    auto drew_pending = false;

    auto append_segment = [&](nxt::utf8::text_segment segment) {
        if (segment.kind == nxt::utf8::text_segment::kind_t::line_break) {
            trim_trailing_space(block);
            block += '\n';
            return;
        }

        block += segment.text;
        block += ' ';
    };

    auto draw_pending = [&] {
        if (block.empty())
            return;
        auto pending = fold_when_done
            ? nxt::tui::AnyLayout{thought_marquee_block(block)}
            : nxt::tui::AnyLayout{pending_stream_block(block, style, wrap_width)};
        if (hud)
            self.draw(hud->view(std::move(pending)));
        else
            self.draw(std::move(pending));
        drew_pending = true;
    };

    auto commit_block = [&] {
        trim_trailing_space(block);
        if (fold_when_done && (!block.empty() || drew_pending)) {
            if (!block.empty()) {
                self.print(markdown_text_block(block, style, wrap_width));
                self.print("\n");
            }
            if (hud) {
                hud->add(folded_thought_block());
                self.draw(hud->view());
            } else {
                self.draw(folded_thought_block());
            }
        } else if (!block.empty()) {
            if (separate_before_commit) {
                self.print("\n");
                self.print(output_separator());
                self.print("\n");
            }
            self.print(markdown_text_block(block, style, wrap_width));
            self.print("\n");
            if (hud)
                self.draw(hud->view());
            else
                self.draw(nxt::tui::text(""));
        } else if (drew_pending) {
            if (hud)
                self.draw(hud->view());
            else
                self.draw(nxt::tui::text(""));
        }
    };

    while (auto event = co_await nxt::next(stream)) {
        const stream_event & current = *event;
        
        if (current.type == delta_event_type) {
            auto payload = current.read<openai::text_delta_event>();

            text += payload.delta;
            for_complete_words(text, false, append_segment);
            draw_pending();
        }

        else if (current.type == "response.output_item.done") {
            auto payload = current.read<openai::output_item_event>();

            for_complete_words(text, true, append_segment);
            commit_block();
            co_return payload.item;
        }

        else if (is_text_item_scaffolding_event(current)) {
            continue;
        }

        else
            throw_unexpected_event("reading output text item", current);
    }

    throw_unexpected_eof("reading output text item");
}

template<typename Stream>
nxt::task<std::optional<openai::raw_json>>
read_reasoning_item(
    Stream & stream,
    nxt::ui::yard & self,
    hud_blocks::State * hud = nullptr)
{
    co_return co_await read_text_delta_item(
        stream,
        self,
        "response.reasoning_summary_text.delta",
        nxt::tui::fg(nxt::Rgba8::cyan()),
        true,
        false,
        hud);
}

template<typename Stream>
nxt::task<std::optional<openai::raw_json>>
read_message_item(
    Stream & stream,
    nxt::ui::yard & self,
    hud_blocks::State * hud = nullptr)
{
    co_return co_await read_text_delta_item(
        stream,
        self,
        "response.output_text.delta",
        nxt::tui::fg(nxt::Rgba8::yellow()),
        false,
        true,
        hud);
}

template<typename Stream>
nxt::task<std::optional<openai::raw_json>> read_output_item(
    Stream & stream,
    nxt::ui::yard & self,
    const stream_event & first,
    hud_blocks::State * hud = nullptr)
{
    auto item_type = glz::get_sv_json<"/item/type">(first.data);
    
    if (item_type == "reasoning")
        co_return co_await read_reasoning_item(stream, self, hud);

    else if (item_type == "message")
        co_return co_await read_message_item(stream, self, hud);

    else if (item_type == "function_call") {
        while (std::optional<stream_event> event = co_await nxt::next(stream)) {
            if (event->type == "response.output_item.done") {
                auto item = event->read<openai::output_item_event>();
                co_return item.item;
            }

            if (is_function_call_item_event(*event))
                continue;

            throw_unexpected_event("reading function call item", *event);
        }

        throw_unexpected_eof("reading function call item");
    }

    else
        throw nxt::io::runtime_error{
            "unexpected OpenAI output item type: " + first.data};
}

template<typename ReadStream>
nxt::task<response_stream_result> with_openai_response_stream(
    nxt::ui::yard & self,
    const llm_request & request,
    ReadStream read_stream)
{
    auto & runtime = self.runtime();
    auto transport = co_await nxt::io::net::connect_tls(
        runtime.scheduler_handle(), openai_responses_endpoint());

    using transport_t = decltype(transport);
    auto stream = responses::openai_response_stream<transport_t>{
        transport, runtime.get_stop_token()};

    co_return co_await run_then_shutdown<response_stream_result>(
        runtime,
        "response stream failed",
        [&]() -> nxt::task<response_stream_result> {
            agent_trace::record_llm_request(self, request);
            co_await stream.connect(request);
            co_return co_await nxt::invoke(std::move(read_stream), stream);
        },
        "transport shutdown failed",
        [&]() {
            return transport.shutdown();
        });
}

template<typename Stream>
nxt::task<response_stream_result>
read_openai_response_stream(
    nxt::ui::yard & self,
    Stream & stream,
    hud_blocks::State * hud = nullptr)
{
    if (hud)
        self.draw(hud->view(status_row("streaming response")));
    else
        self.draw(status_row("streaming response"));

    auto events = response_event_source(self, stream);
    auto result = response_stream_result{};

    while (std::optional<stream_event> event = co_await nxt::next(events)) {
        if (event->type == "response.created") {
            auto payload = event->read<openai::response_event>();
            result.response_id = payload.response.id;
        }
        
        else if (event->type == "response.output_item.added") {
            auto output_item =
                co_await read_output_item(events, self, *event, hud);

            if (output_item)
                result.output_items.push_back(std::move(*output_item));
        }

        else if (event->type == "response.completed") {
            result.completed = true;
            co_return result;
        }

        else if (event->type == "response.failed" || event->type == "response.incomplete") {
            co_return result;
        }

        else if (event->type == "response.in_progress") {
            continue;
        }

        else
            throw_unexpected_event("reading response stream", *event);
    }

    throw_unexpected_eof("reading response stream");
}

inline nxt::task<response_stream_result>
request_response_turn(
    nxt::ui::yard & self,
    const llm_request & request,
    hud_blocks::State * hud = nullptr)
{
    if (hud)
        self.draw(hud->view(status_row("dialing OpenAI")));
    else
        self.draw(status_row("dialing OpenAI"));
    co_return co_await with_openai_response_stream(
        self,
        request,
        [&](auto & stream) {
            return read_openai_response_stream(self, stream, hud);
        });
}

} // namespace nxt::ai::response_turn
