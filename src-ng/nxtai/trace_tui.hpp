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

inline auto waterfall_bar(
    nxt::rt::trace_clock::duration offset,
    nxt::rt::trace_clock::duration duration,
    nxt::rt::trace_clock::duration total,
    Rgba8 accent = tool_tui::sky_300)
{
    auto total_us =
        std::chrono::duration_cast<std::chrono::microseconds>(total).count();
    auto offset_us =
        std::chrono::duration_cast<std::chrono::microseconds>(offset).count();
    auto duration_us =
        std::chrono::duration_cast<std::chrono::microseconds>(duration)
            .count();
    auto begin = total_us > 0
        ? static_cast<double>(offset_us) / static_cast<double>(total_us)
        : 0.0;
    auto end = total_us > 0
        ? static_cast<double>(offset_us + duration_us)
              / static_cast<double>(total_us)
        : 0.0;
    return nxt::tui::range_progress_bar(
        begin, end, accent, tool_tui::slate_800);
}

inline auto waterfall_header(
    const waterfall_view & view,
    const waterfall_options & options)
{
    namespace tt = tool_tui;

    return nxt::tui::row(
        nxt::tui::when(
            !options.label.empty(),
            tt::chip(" " + options.label + " ",
                     tt::slate_950,
                     options.accent,
                     nxt::Emphasis::bold)),
        nxt::tui::when(
            !options.detail.empty(),
            tt::chip(" " + options.detail + " ",
                     options.accent,
                     tt::band_bg,
                     nxt::Emphasis::bold)),
        nxt::tui::flex_text(
            view.subject,
            nxt::tui::fg(tt::slate_300) | nxt::tui::bg(tt::band_bg)),
        tt::chip(
            std::format(" {} ", format_duration(view.total)),
            tt::slate_950,
            tt::amber_300,
            nxt::Emphasis::bold));
}

inline auto waterfall_row_layout(
    const waterfall_row & row,
    nxt::rt::trace_clock::duration total,
    Rgba8 accent)
{
    namespace tt = tool_tui;
    return nxt::tui::row(
        nxt::tui::fixed_width(
            28 * nxt::ch,
            nxt::tui::flex_text(
                row.name,
                nxt::tui::fg(tt::slate_300)
                    | nxt::tui::bg(tt::page_bg))),
        nxt::tui::fixed_width(
            9 * nxt::ch,
            nxt::tui::text(
                std::format("+{:>7}", format_duration(row.offset)),
                nxt::tui::fg(tt::slate_500)
                    | nxt::tui::bg(tt::page_bg))),
        nxt::tui::fixed_width(
            9 * nxt::ch,
            nxt::tui::text(
                std::format("{:>7}  ", format_duration(row.duration)),
                nxt::tui::fg(tt::slate_400)
                    | nxt::tui::bg(tt::page_bg))),
        waterfall_bar(row.offset, row.duration, total, accent));
}

inline auto render_waterfall(
    waterfall_view view,
    waterfall_options options = {})
{
    namespace tt = tool_tui;
    auto no_rows = view.rows.empty();
    auto total = view.total;
    auto accent = options.accent;

    return nxt::tui::surface(
        nxt::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxt::DEFAULT_EMPHASIS,
        },
        tt::block(
            waterfall_header(view, options),
            nxt::tui::either(
                no_rows,
                nxt::tui::each(
                    std::move(view.rows),
                    [total, accent](const waterfall_row & row) {
                        return waterfall_row_layout(row, total, accent);
                    }),
                tt::body_line("no completed child spans", tt::slate_500))));
}

inline auto render_span_waterfall(
    const nxt::rt::trace_context & trace,
    const nxt::rt::trace_span & span,
    waterfall_options options = {})
{
    auto subject = options.subject;
    auto view = collect_waterfall(trace, span, std::move(subject));
    return render_waterfall(std::move(view), std::move(options));
}

} // namespace nxt::ai::trace_tui
