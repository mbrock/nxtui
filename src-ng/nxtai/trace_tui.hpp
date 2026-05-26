#pragma once

#include <nxt/rt/trace.hpp>
#include <nxtai/tool_tui.hpp>

#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::trace_tui {

struct waterfall_row
{
    std::string name;
    nxt::rt::trace_clock::duration offset{};
    nxt::rt::trace_clock::duration duration{};
};

struct waterfall_view
{
    std::string subject;
    nxt::rt::trace_clock::duration total{};
    std::vector<waterfall_row> rows;
};

struct waterfall_options
{
    std::string label = "span";
    std::string detail;
    std::string subject;
    Rgba8 accent = tool_tui::teal_300;
};

inline std::string format_duration(
    nxt::rt::trace_clock::duration duration)
{
    auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(duration)
            .count();
    if (us < 1000)
        return std::format("{}us", us);
    if (us < 1000 * 1000)
        return std::format("{}ms", (us + 500) / 1000);
    return std::format("{:.2f}s", static_cast<double>(us) / 1000000.0);
}

inline std::string display_name(std::string_view name)
{
    auto out = std::string{name};
    for (auto & ch : out) {
        if (ch == '_' || ch == '.')
            ch = ' ';
    }
    return out;
}

inline std::string attribute_value(
    const nxt::rt::trace_attributes & attributes,
    std::string_view key)
{
    for (const auto & attribute : attributes) {
        if (attribute.key == key)
            return attribute.value;
    }
    return {};
}

inline waterfall_view collect_waterfall(
    const nxt::rt::trace_context & trace,
    const nxt::rt::trace_span & span,
    std::string subject = {})
{
    auto children = trace.children(span.span_id());
    auto root = trace.span(span.span_id());
    if (subject.empty() && root)
        subject = display_name(root->name);

    auto start = nxt::rt::trace_clock::time_point{};
    auto end = nxt::rt::trace_clock::time_point{};
    for (const auto & child : children) {
        if (child.end == nxt::rt::trace_clock::time_point{})
            continue;
        if (start == nxt::rt::trace_clock::time_point{}
            || child.start < start)
            start = child.start;
        if (child.end > end)
            end = child.end;
    }
    if (start == nxt::rt::trace_clock::time_point{} && root) {
        start = root->start;
        end = root->end;
    }

    auto view = waterfall_view{
        .subject = std::move(subject),
        .total = end > start ? end - start
                             : nxt::rt::trace_clock::duration{},
        .rows = {},
    };

    for (const auto & child : children) {
        if (child.end == nxt::rt::trace_clock::time_point{})
            continue;
        auto offset = child.start > start
            ? child.start - start
            : nxt::rt::trace_clock::duration{};
        auto duration = child.end > child.start
            ? child.end - child.start
            : nxt::rt::trace_clock::duration{};
        view.rows.push_back(
            waterfall_row{
                .name = display_name(child.name),
                .offset = offset,
                .duration = duration,
            });
    }
    return view;
}

nxt::tui::AnyLayout waterfall_bar(
    nxt::rt::trace_clock::duration offset,
    nxt::rt::trace_clock::duration duration,
    nxt::rt::trace_clock::duration total,
    Rgba8 accent = tool_tui::sky_300);

nxt::tui::AnyLayout waterfall_header(
    const waterfall_view & view,
    const waterfall_options & options);

nxt::tui::AnyLayout waterfall_row_layout(
    const waterfall_row & row,
    nxt::rt::trace_clock::duration total,
    Rgba8 accent);

nxt::tui::AnyLayout render_waterfall(
    waterfall_view view,
    waterfall_options options = {});

nxt::tui::AnyLayout render_span_waterfall(
    const nxt::rt::trace_context & trace,
    const nxt::rt::trace_span & span,
    waterfall_options options = {});

} // namespace nxt::ai::trace_tui
