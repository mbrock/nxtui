#include <nxtai/trace_tui.hpp>

#include <nxtui/tui.hpp>
#include <utility>

namespace nxtai::trace_tui {

using namespace nxtui;

nxtui::tui::AnyLayout waterfall_bar(
    nxtrt::trace_clock::duration offset,
    nxtrt::trace_clock::duration duration,
    nxtrt::trace_clock::duration total,
    Rgba8 accent)
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
    return nxtui::tui::range_progress_bar(
        begin, end, accent, tool_tui::slate_800);
}

nxtui::tui::AnyLayout waterfall_header(
    const waterfall_view & view,
    const waterfall_options & options)
{
    namespace tt = tool_tui;
    auto children = std::vector<nxtui::tui::AnyLayout>{};
    children.reserve(4);

    if (!options.label.empty()) {
        children.push_back(tt::chip(
            " " + options.label + " ",
            tt::slate_950,
            options.accent,
            nxtui::Emphasis::bold));
    }
    if (!options.detail.empty()) {
        children.push_back(tt::chip(
            " " + options.detail + " ",
            options.accent,
            tt::band_bg,
            nxtui::Emphasis::bold));
    }
    children.push_back(nxtui::tui::flex_text(
        view.subject,
        nxtui::tui::fg(tt::slate_300) | nxtui::tui::bg(tt::band_bg)));
    children.push_back(tt::chip(
        std::format(" {} ", format_duration(view.total)),
        tt::slate_950,
        tt::amber_300,
        nxtui::Emphasis::bold));
    return nxtui::tui::row(std::move(children));
}

nxtui::tui::AnyLayout waterfall_row_layout(
    const waterfall_row & row,
    nxtrt::trace_clock::duration total,
    Rgba8 accent)
{
    namespace tt = tool_tui;
    auto children = std::vector<nxtui::tui::AnyLayout>{};
    children.reserve(4);
    children.push_back(nxtui::tui::fixed_width(
        28 * nxtui::ch,
        nxtui::tui::flex_text(
            row.name,
            nxtui::tui::fg(tt::slate_300) | nxtui::tui::bg(tt::page_bg))));
    children.push_back(nxtui::tui::fixed_width(
        9 * nxtui::ch,
        nxtui::tui::text(
            std::format("+{:>7}", format_duration(row.offset)),
            nxtui::tui::fg(tt::slate_500) | nxtui::tui::bg(tt::page_bg))));
    children.push_back(nxtui::tui::fixed_width(
        9 * nxtui::ch,
        nxtui::tui::text(
            std::format("{:>7}  ", format_duration(row.duration)),
            nxtui::tui::fg(tt::slate_400) | nxtui::tui::bg(tt::page_bg))));
    children.push_back(
        waterfall_bar(row.offset, row.duration, total, accent));
    return nxtui::tui::row(std::move(children));
}

nxtui::tui::AnyLayout render_waterfall(
    waterfall_view view,
    waterfall_options options)
{
    namespace tt = tool_tui;
    auto body = nxtui::tui::AnyLayout{};
    if (view.rows.empty()) {
        body = tt::body_line("no completed child spans", tt::slate_500);
    } else {
        auto total = view.total;
        auto accent = options.accent;
        body = nxtui::tui::each(
            std::move(view.rows),
            [total, accent](const waterfall_row & row) {
                return waterfall_row_layout(row, total, accent);
            });
    }

    return nxtui::tui::surface(
        nxtui::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxtui::DEFAULT_EMPHASIS,
        },
        tt::block(waterfall_header(view, options), std::move(body)));
}

nxtui::tui::AnyLayout render_span_waterfall(
    const nxtrt::trace_context & trace,
    const nxtrt::trace_span & span,
    waterfall_options options)
{
    auto subject = options.subject;
    auto view = collect_waterfall(trace, span, std::move(subject));
    return render_waterfall(std::move(view), std::move(options));
}

} // namespace nxtai::trace_tui
