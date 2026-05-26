#include <nxt/llm/common.hpp>

#include <nxt/rt/buffers.hpp>
#include <nxt/rt/http.hpp>
#include <nxt/rt/net.hpp>
#include <nxt/rt/sampling.hpp>
#include <nxt/rt/tls.hpp>
#include <nxt/rt/ui_runtime.hpp>
#include <nxt/tui.hpp>
#include <nxt/http.hpp>
#include <nxt/json.hpp>
#include <nxtai/tool_tui.hpp>
#include <nxtai/trace_tui.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace nxt::ai::llm {
namespace {
struct stream_event
{
    std::string type;
    std::string data;
};

struct live_tool_call
{
    std::string item_id;
    int output_index = -1;
    nxt::ai::tool_tui::call_view view;
};

struct network_hud_state
{
    std::string phase;
    std::size_t socket_rx = 0;
    std::size_t socket_tx = 0;
    double socket_rx_bps = 0.0;
    double socket_tx_bps = 0.0;
    std::size_t sse_events = 0;
};

struct live_tool_delta
{
    std::string item_id;
    int output_index = -1;
    std::string name;
    std::string delta;
    std::optional<std::string> arguments;
};

struct live_tool_added
{
    std::string item_id;
    int output_index = -1;
    std::string call_id;
    std::string type;
    std::string name;
    std::string arguments;
};

nxt::rt::task<std::optional<nxt::json::token>>
next_json_token(nxt::json::string_reader & in)
{
    co_return co_await nxt::json::read_token(in);
}

nxt::rt::task<bool>
take_json_token(nxt::json::string_reader & in, nxt::json::token_kind kind)
{
    auto token = co_await next_json_token(in);
    co_return token && token->kind == kind;
}

nxt::rt::task<bool> skip_json_value(
    nxt::json::string_reader & in,
    nxt::json::token first)
{
    auto depth = 0;
    if (first.kind == nxt::json::token_kind::object_begin
        || first.kind == nxt::json::token_kind::array_begin) {
        depth = 1;
    } else {
        co_return true;
    }

    while (depth > 0) {
        auto token = co_await next_json_token(in);
        if (!token)
            co_return false;
        if (token->kind == nxt::json::token_kind::object_begin
            || token->kind == nxt::json::token_kind::array_begin)
            ++depth;
        else if (
            token->kind == nxt::json::token_kind::object_end
            || token->kind == nxt::json::token_kind::array_end)
            --depth;
    }
    co_return true;
}

nxt::rt::task<bool> skip_next_json_value(nxt::json::string_reader & in)
{
    auto token = co_await next_json_token(in);
    if (!token)
        co_return false;
    co_return co_await skip_json_value(in, std::move(*token));
}

nxt::rt::task<std::optional<std::string>>
read_json_string_token(nxt::json::string_reader & in)
{
    auto token = co_await next_json_token(in);
    if (!token || token->kind != nxt::json::token_kind::string)
        co_return std::nullopt;
    co_return std::move(token->text);
}

nxt::rt::task<std::optional<int>> read_json_int_token(nxt::json::string_reader & in)
{
    auto token = co_await next_json_token(in);
    if (!token || token->kind != nxt::json::token_kind::number)
        co_return std::nullopt;
    auto value = 0;
    auto parsed = std::from_chars(
        token->text.data(), token->text.data() + token->text.size(), value);
    if (parsed.ec != std::errc{})
        co_return std::nullopt;
    co_return value;
}

nxt::rt::task<std::optional<std::string>>
read_response_created_id(std::string_view data)
{
    auto in = nxt::json::string_reader{.input = data};
    if (!(co_await take_json_token(in, nxt::json::token_kind::object_begin)))
        co_return std::nullopt;

    while (true) {
        auto token = co_await next_json_token(in);
        if (!token)
            co_return std::nullopt;
        if (token->kind == nxt::json::token_kind::object_end)
            co_return std::nullopt;
        if (token->kind != nxt::json::token_kind::string)
            co_return std::nullopt;

        auto key = std::move(token->text);
        if (!(co_await take_json_token(in, nxt::json::token_kind::colon)))
            co_return std::nullopt;
        if (key != "response") {
            if (!(co_await skip_next_json_value(in)))
                co_return std::nullopt;
        } else {
            if (!(co_await take_json_token(in, nxt::json::token_kind::object_begin)))
                co_return std::nullopt;
            while (true) {
                token = co_await next_json_token(in);
                if (!token)
                    co_return std::nullopt;
                if (token->kind == nxt::json::token_kind::object_end)
                    break;
                if (token->kind != nxt::json::token_kind::string)
                    co_return std::nullopt;
                auto response_key = std::move(token->text);
                if (!(co_await take_json_token(in, nxt::json::token_kind::colon)))
                    co_return std::nullopt;
                if (response_key == "id")
                    co_return co_await read_json_string_token(in);
                if (!(co_await skip_next_json_value(in)))
                    co_return std::nullopt;
                token = co_await next_json_token(in);
                if (!token)
                    co_return std::nullopt;
                if (token->kind == nxt::json::token_kind::object_end)
                    break;
                if (token->kind != nxt::json::token_kind::comma)
                    co_return std::nullopt;
            }
        }

        token = co_await next_json_token(in);
        if (!token)
            co_return std::nullopt;
        if (token->kind == nxt::json::token_kind::object_end)
            co_return std::nullopt;
        if (token->kind != nxt::json::token_kind::comma)
            co_return std::nullopt;
    }
}

nxt::rt::task<std::optional<nxt::ai::openai::raw_json>>
read_output_item_done_item(std::string_view data)
{
    auto in = nxt::json::string_reader{.input = data};
    if (!(co_await take_json_token(in, nxt::json::token_kind::object_begin)))
        co_return std::nullopt;

    while (true) {
        auto token = co_await next_json_token(in);
        if (!token)
            co_return std::nullopt;
        if (token->kind == nxt::json::token_kind::object_end)
            co_return std::nullopt;
        if (token->kind != nxt::json::token_kind::string)
            co_return std::nullopt;

        auto key = std::move(token->text);
        if (!(co_await take_json_token(in, nxt::json::token_kind::colon)))
            co_return std::nullopt;

        co_await nxt::json::skip_ws(in);
        auto value_begin = in.offset;
        token = co_await next_json_token(in);
        if (!token)
            co_return std::nullopt;
        if (!(co_await skip_json_value(in, *token)))
            co_return std::nullopt;
        auto value_end = in.offset;

        if (key == "item")
            co_return nxt::ai::openai::raw_json{
                std::string{data.substr(value_begin, value_end - value_begin)}};

        token = co_await next_json_token(in);
        if (!token)
            co_return std::nullopt;
        if (token->kind == nxt::json::token_kind::object_end)
            co_return std::nullopt;
        if (token->kind != nxt::json::token_kind::comma)
            co_return std::nullopt;
    }
}

std::optional<std::string> event_string_field(
    std::string_view data,
    std::string_view field)
{
    return nxt::ai::tools::json_string_member(data, field);
}

live_tool_call * find_live_tool_call(
    std::vector<live_tool_call> & calls,
    std::string_view item_id,
    int output_index)
{
    for (auto & call : calls) {
        if (!item_id.empty() && call.item_id == item_id)
            return &call;
        if (output_index >= 0 && call.output_index == output_index)
            return &call;
    }
    return nullptr;
}

live_tool_call & ensure_live_tool_call(
    std::vector<live_tool_call> & calls,
    std::string item_id,
    int output_index)
{
    if (auto * existing = find_live_tool_call(calls, item_id, output_index))
        return *existing;
    calls.push_back(
        live_tool_call{
            .item_id = std::move(item_id),
            .output_index = output_index,
            .view =
                nxt::ai::tool_tui::call_view{
                    .name = {},
                    .arguments = {},
                    .output = {},
                    .latest_memory_current = std::nullopt,
                    .state = nxt::ai::tool_tui::status::running,
                    .elapsed_ms = -1,
                },
        });
    return calls.back();
}

nxt::rt::task<bool> read_live_tool_item(
    nxt::json::string_reader & in,
    live_tool_added & out)
{
    if (!(co_await take_json_token(in, nxt::json::token_kind::object_begin)))
        co_return false;
    while (true) {
        auto token = co_await next_json_token(in);
        if (!token)
            co_return false;
        if (token->kind == nxt::json::token_kind::object_end)
            co_return true;
        if (token->kind != nxt::json::token_kind::string)
            co_return false;

        auto key = std::move(token->text);
        if (!(co_await take_json_token(in, nxt::json::token_kind::colon)))
            co_return false;
        if (key == "id") {
            out.item_id =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "type") {
            out.type =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "call_id") {
            out.call_id =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "name") {
            out.name =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "arguments") {
            out.arguments =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (!(co_await skip_next_json_value(in))) {
            co_return false;
        }
        auto separator = co_await next_json_token(in);
        if (!separator)
            co_return false;
        if (separator->kind == nxt::json::token_kind::object_end)
            co_return true;
        if (separator->kind != nxt::json::token_kind::comma)
            co_return false;
    }
}

nxt::rt::task<std::optional<live_tool_added>>
read_live_tool_added(std::string_view data)
{
    auto in = nxt::json::string_reader{.input = data};
    auto out = live_tool_added{};
    if (!(co_await take_json_token(in, nxt::json::token_kind::object_begin)))
        co_return std::nullopt;
    while (true) {
        auto token = co_await next_json_token(in);
        if (!token)
            co_return std::nullopt;
        if (token->kind == nxt::json::token_kind::object_end) {
            co_return out.type == "function_call"
                ? std::optional<live_tool_added>{std::move(out)}
                : std::nullopt;
        }
        if (token->kind != nxt::json::token_kind::string)
            co_return std::nullopt;

        auto key = std::move(token->text);
        if (!(co_await take_json_token(in, nxt::json::token_kind::colon)))
            co_return std::nullopt;
        if (key == "output_index") {
            out.output_index = (co_await read_json_int_token(in)).value_or(-1);
        } else if (key == "item") {
            if (!(co_await read_live_tool_item(in, out)))
                co_return std::nullopt;
        } else if (!(co_await skip_next_json_value(in))) {
            co_return std::nullopt;
        }
        auto separator = co_await next_json_token(in);
        if (!separator)
            co_return std::nullopt;
        if (separator->kind == nxt::json::token_kind::object_end) {
            co_return out.type == "function_call"
                ? std::optional<live_tool_added>{std::move(out)}
                : std::nullopt;
        }
        if (separator->kind != nxt::json::token_kind::comma)
            co_return std::nullopt;
    }
}

nxt::rt::task<std::optional<live_tool_delta>>
read_live_tool_delta(std::string_view data)
{
    auto in = nxt::json::string_reader{.input = data};
    auto out = live_tool_delta{};
    if (!(co_await take_json_token(in, nxt::json::token_kind::object_begin)))
        co_return std::nullopt;
    while (true) {
        auto token = co_await next_json_token(in);
        if (!token)
            co_return std::nullopt;
        if (token->kind == nxt::json::token_kind::object_end) {
            if (out.delta.empty() && !out.arguments)
                co_return std::nullopt;
            co_return out;
        }
        if (token->kind != nxt::json::token_kind::string)
            co_return std::nullopt;

        auto key = std::move(token->text);
        if (!(co_await take_json_token(in, nxt::json::token_kind::colon)))
            co_return std::nullopt;
        if (key == "item_id") {
            out.item_id =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "output_index") {
            out.output_index = (co_await read_json_int_token(in)).value_or(-1);
        } else if (key == "name") {
            out.name =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "delta") {
            out.delta =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (key == "arguments") {
            out.arguments =
                (co_await read_json_string_token(in)).value_or(std::string{});
        } else if (!(co_await skip_next_json_value(in))) {
            co_return std::nullopt;
        }
        auto separator = co_await next_json_token(in);
        if (!separator)
            co_return std::nullopt;
        if (separator->kind == nxt::json::token_kind::object_end) {
            if (out.delta.empty() && !out.arguments)
                co_return std::nullopt;
            co_return out;
        }
        if (separator->kind != nxt::json::token_kind::comma)
            co_return std::nullopt;
    }
}

nxt::rt::task<bool> update_live_tool_call_from_added(
    std::vector<live_tool_call> & calls,
    std::string_view data)
{
    auto added = co_await read_live_tool_added(data);
    if (!added)
        co_return false;
    auto & call = ensure_live_tool_call(
        calls, std::move(added->item_id), added->output_index);
    if (!added->name.empty())
        call.view.name = std::move(added->name);
    if (!added->call_id.empty() && call.item_id.empty())
        call.item_id = std::move(added->call_id);
    if (!added->arguments.empty())
        call.view.arguments = std::move(added->arguments);
    co_return true;
}

nxt::rt::task<bool> update_live_tool_call_from_delta(
    std::vector<live_tool_call> & calls,
    std::string_view data)
{
    auto delta = co_await read_live_tool_delta(data);
    if (!delta)
        co_return false;

    auto & call = ensure_live_tool_call(
        calls, std::move(delta->item_id), delta->output_index);
    if (!delta->name.empty())
        call.view.name = std::move(delta->name);
    if (delta->arguments)
        call.view.arguments = std::move(*delta->arguments);
    else
        call.view.arguments += delta->delta;
    co_return true;
}

[[nodiscard]] bool response_status_is_success(
    const nxt::rt::http::response_head & head)
{
    return head.status >= 200 && head.status < 300;
}

[[nodiscard]] bool response_content_type_is(
    const nxt::rt::http::response_head & head,
    std::string_view expected)
{
    auto value = nxt::rt::http::header_value(head, "content-type");
    if (!value)
        return false;
    auto semicolon = value->find(';');
    auto media_type = nxt::rt::http::trim_ascii(value->substr(0, semicolon));
    return nxt::rt::http::iequals(media_type, expected);
}

std::vector<std::string> last_lines(std::string_view text, std::size_t max)
{
    auto lines = std::vector<std::string>{};
    auto current = std::string{};
    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current = {};
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty() || lines.empty())
        lines.push_back(std::move(current));

    if (lines.size() > max)
        lines.erase(lines.begin(), lines.begin() + (lines.size() - max));
    return lines;
}

std::string join_lines(const std::vector<std::string> & lines)
{
    auto out = std::string{};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0)
            out += '\n';
        out += lines[i];
    }
    return out;
}

std::string format_compact_bytes(std::size_t bytes)
{
    if (bytes < 1000)
        return std::format("{}", bytes);
    auto kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 10.0)
        return std::format("{:.1f}K", kb);
    if (kb < 1000.0)
        return std::format("{:.0f}K", kb);
    auto mb = kb / 1024.0;
    if (mb < 10.0)
        return std::format("{:.1f}M", mb);
    return std::format("{:.0f}M", mb);
}

std::string format_compact_rate(double bytes_per_second)
{
    return format_compact_bytes(
        static_cast<std::size_t>(std::max(bytes_per_second, 0.0))) + "/s";
}

nxt::Rgba8 blend(nxt::Rgba8 a, nxt::Rgba8 b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    auto channel = [=](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(
            static_cast<double>(x)
            + (static_cast<double>(y) - static_cast<double>(x)) * t);
    };
    return nxt::Rgba8{
        channel(a.r(), b.r()),
        channel(a.g(), b.g()),
        channel(a.b(), b.b()),
    };
}

nxt::Rgba8 rate_bg(nxt::Rgba8 color, double bytes_per_second)
{
    static constexpr auto max_display_rate = 32.0 * 1024.0;
    auto fraction =
        std::clamp(bytes_per_second / max_display_rate, 0.0, 1.0);
    return blend(nxt::ai::tool_tui::slate_900, color, 0.12 + 0.58 * fraction);
}

enum class cell_align { left, right };

std::string fit_cell(std::string s, std::size_t width, cell_align align)
{
    if (width == 0)
        return {};
    auto display_width = static_cast<std::size_t>(nxt::tui::utf8_width(s).count());
    if (display_width > width) {
        if (width == 1)
            return s.substr(0, 1);
        s.resize(width - 1);
        s += "…";
        return s;
    }

    auto pad = std::string(width - display_width, ' ');
    if (align == cell_align::right)
        return pad + std::move(s);
    s += pad;
    return s;
}

auto fixed_cell(
    nxt::width_t width,
    std::string s,
    nxt::tui::Style style,
    cell_align align = cell_align::left)
{
    auto cells = static_cast<std::size_t>(width.count());
    return nxt::tui::line_text(
        nxt::tui::WidthHint::fixed(width),
        [s = std::move(s), cells, align](nxt::width_t) {
            return fit_cell(s, cells, align);
        },
        style);
}

auto rate_cell(std::string label, double bytes_per_second, nxt::Rgba8 color)
{
    namespace tt = nxt::ai::tool_tui;
    auto style = nxt::tui::fg(tt::slate_300) | nxt::tui::bg(rate_bg(color, bytes_per_second));
    return nxt::tui::line_text(
        nxt::tui::WidthHint{7 * nxt::ch, 1.0 * nxt::one},
        [label = std::move(label)](nxt::width_t width) {
            return fit_cell(label, width.count(), cell_align::right);
        },
        style);
}

void append_thought_delta(std::string & thought, std::string_view delta)
{
    static constexpr auto max_thought_bytes = std::size_t{4096};
    thought.append(delta);
    if (thought.size() <= max_thought_bytes)
        return;
    auto drop = thought.size() - max_thought_bytes;
    auto newline = thought.find('\n', drop);
    if (newline != std::string::npos)
        drop = newline + 1;
    thought.erase(0, drop);
}

auto header_layout(std::string_view model, std::string_view status)
{
    namespace tt = nxt::ai::tool_tui;
    auto summary = std::format("{}  {}", model, status);
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(2);
    children.push_back(tt::chip(
        " nxtllm ",
        tt::slate_950,
        tt::amber_300,
        nxt::Emphasis::bold));
    children.push_back(nxt::tui::flex_text(
        std::move(summary),
        nxt::tui::fg(tt::slate_400) | nxt::tui::bg(tt::band_bg)));
    return nxt::tui::row(std::move(children));
}

nxt::tui::AnyLayout assistant_preview_layout(std::string_view assistant_text)
{
    namespace tt = nxt::ai::tool_tui;
    if (assistant_text.empty())
        return {};
    auto preview = join_lines(last_lines(assistant_text, 8));
    return nxt::tui::text_lines(
        std::move(preview), nxt::tui::fg(tt::slate_300));
}

nxt::tui::AnyLayout stream_activity_layout(
    std::string_view thought,
    std::string_view assistant_text,
    const std::vector<live_tool_call> & live_calls)
{
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(3);
    if (!thought.empty())
        children.push_back(
            nxt::ai::tool_tui::thought_block(std::string{thought}));
    if (!assistant_text.empty())
        children.push_back(assistant_preview_layout(assistant_text));
    if (!live_calls.empty()) {
        children.push_back(nxt::tui::each(
            std::vector<live_tool_call>{live_calls},
            [](const live_tool_call & call) {
                return nxt::ai::tool_tui::render_call(call.view);
            }));
    }
    return nxt::tui::column(std::move(children));
}

nxt::tui::AnyLayout network_footer_layout(const network_hud_state & net)
{
    namespace tt = nxt::ai::tool_tui;
    if (net.phase.empty() && net.socket_rx == 0 && net.socket_tx == 0)
        return {};

    auto phase = net.phase.empty() ? std::string{"network"} : net.phase;
    auto value_style = nxt::tui::fg(tt::slate_300) | nxt::tui::bg(tt::page_bg);
    auto phase_style = nxt::tui::fg(tt::teal_300) | nxt::tui::bg(tt::page_bg)
                     | nxt::tui::em(nxt::Emphasis::bold);
    auto event_style = nxt::tui::fg(tt::slate_500) | nxt::tui::bg(tt::page_bg);
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(13);
    children.push_back(nxt::tui::hfill(1 * nxt::ch, tt::page_bg));
    children.push_back(fixed_cell(14 * nxt::ch, std::move(phase), phase_style));
    children.push_back(nxt::tui::hfill(1 * nxt::ch, tt::page_bg));
    children.push_back(fixed_cell(
        7 * nxt::ch,
        format_compact_bytes(net.socket_rx) + "↓",
        value_style,
        cell_align::right));
    children.push_back(nxt::tui::hfill(1 * nxt::ch, tt::page_bg));
    children.push_back(rate_cell(
        format_compact_rate(net.socket_rx_bps) + "↓",
        net.socket_rx_bps,
        tt::teal_300));
    children.push_back(nxt::tui::hfill(1 * nxt::ch, tt::page_bg));
    children.push_back(fixed_cell(
        7 * nxt::ch,
        format_compact_bytes(net.socket_tx) + "↑",
        value_style,
        cell_align::right));
    children.push_back(nxt::tui::hfill(1 * nxt::ch, tt::page_bg));
    children.push_back(rate_cell(
        format_compact_rate(net.socket_tx_bps) + "↑",
        net.socket_tx_bps,
        tt::amber_300));
    children.push_back(nxt::tui::hfill(1 * nxt::ch, tt::page_bg));
    children.push_back(fixed_cell(
        5 * nxt::ch,
        net.sse_events == 0
            ? std::string{}
            : std::format("#{}", net.sse_events),
        event_style,
        cell_align::right));
    children.push_back(nxt::tui::flex_text("", nxt::tui::bg(tt::page_bg)));
    return nxt::tui::row(std::move(children));
}

nxt::tui::AnyLayout agent_layout(
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    nxt::tui::AnyLayout child)
{
    namespace tt = nxt::ai::tool_tui;
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(3);
    children.push_back(header_layout(model, status));
    children.push_back(assistant_preview_layout(assistant_text));
    children.push_back(std::move(child));
    return nxt::tui::surface(
        nxt::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxt::DEFAULT_EMPHASIS,
        },
        nxt::tui::column(std::move(children)));
}

nxt::tui::AnyLayout stream_layout(
    std::string_view thought,
    std::string_view assistant_text,
    const std::vector<live_tool_call> & live_calls,
    const network_hud_state & network)
{
    namespace tt = nxt::ai::tool_tui;
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(2);
    children.push_back(
        stream_activity_layout(thought, assistant_text, live_calls));
    children.push_back(network_footer_layout(network));
    return nxt::tui::surface(
        nxt::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxt::DEFAULT_EMPHASIS,
        },
        nxt::tui::column(std::move(children)));
}

nxt::rt::task<void> publish_stream_view(
    std::string_view thought,
    std::string_view assistant_text,
    const std::vector<live_tool_call> & live_calls,
    network_hud_state & network,
    nxt::rt::ema_rate & rx_rate,
    nxt::rt::ema_rate & tx_rate,
    std::size_t & last_rx,
    std::size_t & last_tx,
    std::chrono::steady_clock::time_point & last_sample,
    std::chrono::steady_clock::time_point & last_publish,
    bool force = false)
{
    if (!nxt::rt::has_terminal_surface())
        co_return;

    auto now = std::chrono::steady_clock::now();
    if (!force
        && last_publish != std::chrono::steady_clock::time_point{}
        && now - last_publish < frame_interval)
        co_return;

    auto next_rx = network.socket_rx;
    auto next_tx = network.socket_tx;
    network.socket_rx_bps =
        rx_rate.sample(next_rx >= last_rx ? next_rx - last_rx : 0,
                       now - last_sample);
    network.socket_tx_bps =
        tx_rate.sample(next_tx >= last_tx ? next_tx - last_tx : 0,
                       now - last_sample);
    last_rx = next_rx;
    last_tx = next_tx;
    last_sample = now;
    last_publish = now;
    co_await nxt::rt::draw(
        stream_layout(thought, assistant_text, live_calls, network));
}

struct stream_view_publisher
{
    std::string & thought;
    std::string & assistant_text;
    std::vector<live_tool_call> & live_calls;
    network_hud_state & network;
    nxt::rt::ema_rate & rx_rate;
    nxt::rt::ema_rate & tx_rate;
    std::size_t last_rx;
    std::size_t last_tx;
    std::chrono::steady_clock::time_point last_sample;
    std::chrono::steady_clock::time_point last_publish{};
    bool pending = false;
    bool force_pending = false;

    void request(bool force = false) noexcept
    {
        pending = true;
        force_pending = force_pending || force;
    }

    nxt::rt::task<void> publish(bool force = false)
    {
        pending = false;
        force_pending = false;
        co_await publish_stream_view(
            thought,
            assistant_text,
            live_calls,
            network,
            rx_rate,
            tx_rate,
            last_rx,
            last_tx,
            last_sample,
            last_publish,
            force);
    }

    nxt::rt::task<void> flush()
    {
        if (!pending)
            co_return;
        auto force = force_pending;
        pending = false;
        force_pending = false;
        co_await publish_stream_view(
            thought,
            assistant_text,
            live_calls,
            network,
            rx_rate,
            tx_rate,
            last_rx,
            last_tx,
            last_sample,
            last_publish,
            force);
    }
};

stream_view_publisher make_stream_view_publisher(
    std::string & thought,
    std::string & assistant_text,
    std::vector<live_tool_call> & live_calls,
    network_hud_state & network,
    nxt::rt::ema_rate & rx_rate,
    nxt::rt::ema_rate & tx_rate)
{
    return stream_view_publisher{
        .thought = thought,
        .assistant_text = assistant_text,
        .live_calls = live_calls,
        .network = network,
        .rx_rate = rx_rate,
        .tx_rate = tx_rate,
        .last_rx = network.socket_rx,
        .last_tx = network.socket_tx,
        .last_sample = std::chrono::steady_clock::now(),
    };
}

void note_tls_progress(
    const nxt::rt::tls::handshake_progress & progress,
    const std::shared_ptr<nxt::rt::trace_context> & trace,
    const nxt::rt::trace_span & tls_span,
    std::vector<nxt::rt::trace_span> & active_tls_spans,
    network_hud_state & network)
{
    if (progress.kind == nxt::rt::tls::handshake_progress_kind::begin) {
        network.phase = std::format(
            "TLS {}",
            nxt::ai::trace_tui::display_name(progress.name));
        if (trace != nullptr && tls_span)
            active_tls_spans.push_back(
                trace->start_span(
                    std::string{progress.name},
                    tls_span.span_id()));
        return;
    }

    if (progress.kind == nxt::rt::tls::handshake_progress_kind::end) {
        for (auto it = active_tls_spans.rbegin();
             it != active_tls_spans.rend();
             ++it) {
            if (it->name() == progress.name) {
                it->finish("ok");
                active_tls_spans.erase(std::next(it).base());
                break;
            }
        }
        return;
    }

    if (trace != nullptr && tls_span)
        tls_span.event(std::string{progress.name});
    network.phase = std::format(
        "TLS {}",
        nxt::ai::trace_tui::display_name(progress.name));
}

nxt::rt::task<void> print_tls_ready(
    const std::shared_ptr<nxt::rt::trace_context> & trace,
    const nxt::rt::trace_span & tls_span)
{
    if (!nxt::rt::has_terminal_surface())
        co_return;

    if (trace != nullptr && tls_span) {
        co_await nxt::rt::print(
            nxt::ai::trace_tui::render_span_waterfall(
                *trace,
                tls_span,
                {
                    .label = "",
                    .detail = "TLS 1.3",
                    .subject = "api.openai.com",
                    .accent = nxt::ai::tool_tui::teal_300,
                }));
    } else {
        co_await nxt::rt::print_block(
            "tls  api.openai.com  TLS 1.3 handshake\n");
    }
    co_return;
}

} // namespace

nxt::rt::task<stream_phase_result> stream_openai_response(
    const llm_request & request)
{
    auto thought = std::string{};
    auto assistant_text = std::string{};
    auto live_calls = std::vector<live_tool_call>{};
    auto network = network_hud_state{.phase = "connecting"};
    auto rx_rate = nxt::rt::ema_rate{std::chrono::milliseconds{700}};
    auto tx_rate = nxt::rt::ema_rate{std::chrono::milliseconds{700}};
    auto publisher = make_stream_view_publisher(
        thought, assistant_text, live_calls, network, rx_rate, tx_rate);
    co_await publisher.publish(true);

    auto socket = co_await nxt::rt::net::connect_tcp("api.openai.com", "443");
    network.phase = "tcp connected";
    co_await publisher.publish(true);
    auto socket_sink = nxt::rt::socket_sink{socket.get()};
    auto metered_sink = nxt::rt::meter_sink(
        socket_sink,
        [&](std::size_t bytes) {
            network.socket_tx += bytes;
            publisher.request();
        });
    auto socket_output = nxt::rt::byte_writer{metered_sink, 4096};

    auto socket_source = nxt::rt::socket_source{socket.get()};
    auto source = nxt::rt::meter_source(
        socket_source,
        [&](std::size_t bytes) {
            network.socket_rx += bytes;
            publisher.request();
        });
    auto input_storage = std::vector<std::byte>(18 * 1024);
    auto reader = nxt::rt::byte_reader{
        source,
        std::span{input_storage},
    };

    auto tls = nxt::rt::tls::tls13_client_session{reader, socket_output};
    network.phase = "TLS handshake";
    co_await publisher.publish(true);
    auto trace = nxt::rt::current_trace_context();
    auto tls_span = nxt::rt::trace_span{};
    if (trace != nullptr) {
        tls_span = trace->start_span(
            "tls.handshake",
            nxt::rt::current_trace_span_id(),
            {
                {"net.peer.name", "api.openai.com"},
                {"tls.version", "1.3"},
            });
    }
    auto active_tls_spans = std::vector<nxt::rt::trace_span>{};
    try {
        auto progress = [&](
            const nxt::rt::tls::handshake_progress & step) {
            note_tls_progress(
                step, trace, tls_span, active_tls_spans, network);
            publisher.request(true);
        };
        co_await tls.handshake("api.openai.com", progress);
        co_await publisher.flush();
        if (tls_span)
            tls_span.finish("ok");
    } catch (...) {
        for (auto & span : active_tls_spans)
            span.finish("error");
        if (tls_span)
            tls_span.finish("error");
        throw;
    }
    co_await print_tls_ready(trace, tls_span);
    network.phase = "TLS ready";
    co_await publisher.publish(true);

    auto http_request =
        nxt::ai::responses::openai_responses_http_request(request);
    for (auto & header : http_request.headers) {
        if (header.name == "Connection")
            header.value = "close";
    }
    auto request_text = nxt::http::serialize(http_request);
    network.phase = "http request";
    co_await publisher.publish(true);
    co_await tls.write_all(request_text);
    co_await publisher.flush();

    auto http_storage = std::vector<std::byte>(18 * 1024);
    auto http_reader =
        nxt::rt::byte_reader{tls, std::span{http_storage}};
    network.phase = "http response";
    co_await publisher.publish(true);
    auto head = co_await nxt::rt::http::read_response_head(http_reader);
    co_await publisher.flush();
    if (!response_status_is_success(head))
        throw nxt::rt::runtime_error{
            "OpenAI Responses HTTP error: " + std::to_string(head.status)
            + " " + head.reason};
    if (!response_content_type_is(head, "text/event-stream"))
        throw nxt::rt::runtime_error{
            "OpenAI Responses expected text/event-stream"};

    auto body = nxt::rt::http::read_response_body(http_reader, head);
    auto sse_storage = std::vector<std::byte>(18 * 1024);
    auto sse_reader = nxt::rt::byte_reader{body, std::span{sse_storage}};
    auto result = response_stream_result{};
    network.phase = "streaming SSE";
    co_await publisher.publish(true);
    while (auto sse = co_await nxt::rt::http::parse_sse_event(sse_reader)) {
        co_await publisher.flush();
        if (sse->data == "[DONE]")
            break;
        ++network.sse_events;
        co_await publisher.publish();

        auto terminal = sse->type == "response.completed"
                        || sse->type == "response.incomplete"
                        || sse->type == "response.failed";
        auto event = stream_event{
            .type = std::move(sse->type),
            .data = std::move(sse->data),
        };

        if (event.type == "response.created") {
            if (auto id = co_await read_response_created_id(event.data);
                id && !id->empty()) {
                result.response_id = std::move(*id);
            }
        } else if (event.type == "response.output_item.added") {
            if (co_await update_live_tool_call_from_added(live_calls, event.data))
                co_await publisher.publish(true);
        } else if (event.type == "response.function_call_arguments.delta") {
            if (co_await update_live_tool_call_from_delta(live_calls, event.data))
                co_await publisher.publish();
        } else if (event.type == "response.function_call_arguments.done") {
            if (co_await update_live_tool_call_from_delta(live_calls, event.data))
                co_await publisher.publish(true);
        } else if (event.type == "response.output_item.done") {
            if (auto item = co_await read_output_item_done_item(event.data))
                result.output_items.push_back(std::move(*item));
        } else if (event.type == "response.reasoning_summary_part.added") {
            co_await publisher.publish(true);
        } else if (event.type == "response.reasoning_summary_text.delta") {
            auto text = event_string_field(event.data, "delta");
            if (text)
                append_thought_delta(thought, *text);
            co_await publisher.publish();
        } else if (event.type == "response.reasoning_summary_text.done") {
            auto text = event_string_field(event.data, "text");
            if (text && !text->empty())
                thought = std::move(*text);
            auto summary = std::move(thought);
            thought.clear();
            if (nxt::rt::has_terminal_surface() && !summary.empty())
                co_await nxt::rt::print(
                    nxt::ai::tool_tui::thought_block(std::move(summary)));
            co_await publisher.publish(true);
        } else if (event.type == "response.output_text.delta") {
            auto first_stream_delta = assistant_text.empty();
            auto text = event_string_field(event.data, "delta")
                            .value_or(std::string{});
            assistant_text += text;
            if (nxt::rt::has_terminal_surface()) {
                co_await publisher.publish(first_stream_delta);
            } else {
                co_await nxt::rt::write_stdout_all(std::move(text));
            }
        } else if (
            event.type == "response.failed"
            || event.type == "response.incomplete") {
            throw nxt::rt::runtime_error{
                "OpenAI Responses terminal event: " + event.type};
        }

        if (event.type == "response.completed")
            result.completed = true;
        if (terminal)
            break;
    }

    if (!nxt::rt::has_terminal_surface())
        co_await nxt::rt::write_stdout_all("\n");
    co_return stream_phase_result{
        .response = std::move(result),
        .assistant_text = std::move(assistant_text),
    };
}

template<typename T>
T take_phase_result(nxt::rt::catching_deed<T> deed)
{
    auto result = std::move(deed).get();
    if (result)
        return std::move(*result);
    nxt::rt::rethrow(result.error());
}

nxt::rt::task<nxt::rt::catching_deed<stream_phase_result>>
spawn_stream_phase_child(
    const llm_request & request,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text)
{
    auto child = nxt::rt::spawn_widget(
        [&request] {
            return stream_openai_response(request);
        });
    co_await nxt::rt::draw(
        agent_layout(
            model, status, assistant_text, child.surface()));
    co_return std::move(child).cope();
}

nxt::rt::task<stream_phase_result> run_stream_phase(
    const llm_request & request,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text)
{
    auto deed = co_await nxt::rt::with_zone(
        [&] {
            return spawn_stream_phase_child(
                request,
                model,
                status,
                assistant_text);
        });
    co_return take_phase_result(std::move(deed));
}


} // namespace nxt::ai::llm
