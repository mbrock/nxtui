#pragma once

#include <nxtrt/trace.hpp>
#include <nxtai/tool_tui.hpp>

#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @namespace nxtai::trace_tui
 * Waterfall view of an `nxtrt` trace span: one row per completed child
 * span, with a bar showing when it ran relative to its siblings.
 * `render_span_waterfall` is the entry point.
 */
namespace nxtai::trace_tui {

using namespace nxtui;

/// One child span: display name, start offset from the earliest child,
/// and duration.
struct waterfall_row
{
    std::string name;
    nxtrt::trace_clock::duration offset{};
    nxtrt::trace_clock::duration duration{};
};

/// Rows and overall time span for a waterfall.
struct waterfall_view
{
    std::string subject;
    nxtrt::trace_clock::duration total{};
    std::vector<waterfall_row> rows;
};

/// Header label, detail, subject override, and bar color.
struct waterfall_options
{
    std::string label = "span";
    std::string detail;
    std::string subject;
    Rgba8 accent = tool_tui::teal_300;
};

inline std::string format_duration(
    nxtrt::trace_clock::duration duration)
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
    const nxtrt::trace_attributes & attributes,
    std::string_view key)
{
    for (const auto & attribute : attributes) {
        if (attribute.key == key)
            return attribute.value;
    }
    return {};
}

/// Collect the completed children of `span` from `trace`. The time span
/// runs from the earliest child start to the latest child end, or covers
/// the span itself when no child has finished.
inline waterfall_view collect_waterfall(
    const nxtrt::trace_context & trace,
    const nxtrt::trace_span & span,
    std::string subject = {})
{
    auto children = trace.children(span.span_id());
    auto root = trace.span(span.span_id());
    if (subject.empty() && root)
        subject = display_name(root->name);

    auto start = nxtrt::trace_clock::time_point{};
    auto end = nxtrt::trace_clock::time_point{};
    for (const auto & child : children) {
        if (child.end == nxtrt::trace_clock::time_point{})
            continue;
        if (start == nxtrt::trace_clock::time_point{}
            || child.start < start)
            start = child.start;
        if (child.end > end)
            end = child.end;
    }
    if (start == nxtrt::trace_clock::time_point{} && root) {
        start = root->start;
        end = root->end;
    }

    auto view = waterfall_view{
        .subject = std::move(subject),
        .total = end > start ? end - start
                             : nxtrt::trace_clock::duration{},
        .rows = {},
    };

    for (const auto & child : children) {
        if (child.end == nxtrt::trace_clock::time_point{})
            continue;
        auto offset = child.start > start
            ? child.start - start
            : nxtrt::trace_clock::duration{};
        auto duration = child.end > child.start
            ? child.end - child.start
            : nxtrt::trace_clock::duration{};
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
    nxtrt::trace_clock::duration offset,
    nxtrt::trace_clock::duration duration,
    nxtrt::trace_clock::duration total,
    Rgba8 accent = tool_tui::sky_300)
{
    using std::chrono::duration_cast;
    using std::chrono::microseconds;
    auto total_us = duration_cast<microseconds>(total).count();
    auto offset_us = duration_cast<microseconds>(offset).count();
    auto duration_us = duration_cast<microseconds>(duration).count();
    auto begin = total_us > 0
        ? static_cast<double>(offset_us) / static_cast<double>(total_us)
        : 0.0;
    auto end = total_us > 0
        ? static_cast<double>(offset_us + duration_us)
              / static_cast<double>(total_us)
        : 0.0;
    return tui::range_progress_bar(begin, end, accent, tool_tui::slate_800);
}

inline auto waterfall_header(
    const waterfall_view & view,
    const waterfall_options & options)
{
    namespace tt = tool_tui;
    return tui::row(
        tui::when(
            !options.label.empty(),
            tt::chip(
                " " + options.label + " ",
                tt::slate_950,
                options.accent,
                Emphasis::bold)),
        tui::when(
            !options.detail.empty(),
            tt::chip(
                " " + options.detail + " ",
                options.accent,
                tt::band_bg,
                Emphasis::bold)),
        tui::flex_text(view.subject, tui::fg(tt::slate_300) | tui::bg(tt::band_bg)),
        tt::chip(
            std::format(" {} ", format_duration(view.total)),
            tt::slate_950,
            tt::amber_300,
            Emphasis::bold));
}

inline auto waterfall_row_layout(
    const waterfall_row & row,
    nxtrt::trace_clock::duration total,
    Rgba8 accent)
{
    namespace tt = tool_tui;
    return tui::row(
        tui::fixed_width(
            28 * ch,
            tui::flex_text(row.name, tui::fg(tt::slate_300) | tui::bg(tt::page_bg))),
        tui::fixed_width(
            9 * ch,
            tui::text(
                std::format("+{:>7}", format_duration(row.offset)),
                tui::fg(tt::slate_500) | tui::bg(tt::page_bg))),
        tui::fixed_width(
            9 * ch,
            tui::text(
                std::format("{:>7}  ", format_duration(row.duration)),
                tui::fg(tt::slate_400) | tui::bg(tt::page_bg))),
        waterfall_bar(row.offset, row.duration, total, accent));
}

inline auto render_waterfall(
    waterfall_view view,
    waterfall_options options = {})
{
    namespace tt = tool_tui;
    auto header = waterfall_header(view, options);
    auto total = view.total;
    auto accent = options.accent;
    return tui::surface(
        tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = DEFAULT_EMPHASIS,
        },
        tt::block(
            std::move(header),
            tui::either(
                view.rows.empty(),
                [&] {
                    return tui::each(
                        std::move(view.rows),
                        [total, accent](const waterfall_row & row) {
                            return waterfall_row_layout(row, total, accent);
                        });
                },
                [] {
                    return tt::body_line(
                        "no completed child spans", tt::slate_500);
                })));
}

/// Layout of `collect_waterfall(trace, span)` with a header from
/// `options`. The layout owns its data.
inline auto render_span_waterfall(
    const nxtrt::trace_context & trace,
    const nxtrt::trace_span & span,
    waterfall_options options = {})
{
    auto subject = options.subject;
    auto view = collect_waterfall(trace, span, std::move(subject));
    return render_waterfall(std::move(view), std::move(options));
}

} // namespace nxtai::trace_tui
