#include <nxt/llm/trace_tui.hpp>

#include <nxt/tui.hpp>
#include <utility>

namespace nxt::llm::trace_tui {

nxt::tui::AnyLayout waterfall_bar(
    nxt::rt::trace_clock::duration offset,
    nxt::rt::trace_clock::duration duration,
    nxt::rt::trace_clock::duration total,
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
    return nxt::tui::range_progress_bar(
        begin, end, accent, tool_tui::slate_800);
}

nxt::tui::AnyLayout waterfall_header(
    const waterfall_view & view,
    const waterfall_options & options)
{
    namespace tt = tool_tui;
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(4);

    if (!options.label.empty()) {
        children.push_back(tt::chip(
            " " + options.label + " ",
            tt::slate_950,
            options.accent,
            nxt::Emphasis::bold));
    }
    if (!options.detail.empty()) {
        children.push_back(tt::chip(
            " " + options.detail + " ",
            options.accent,
            tt::band_bg,
            nxt::Emphasis::bold));
    }
    children.push_back(nxt::tui::flex_text(
        view.subject,
        nxt::tui::fg(tt::slate_300) | nxt::tui::bg(tt::band_bg)));
    children.push_back(tt::chip(
        std::format(" {} ", format_duration(view.total)),
        tt::slate_950,
        tt::amber_300,
        nxt::Emphasis::bold));
    return nxt::tui::row(std::move(children));
}

nxt::tui::AnyLayout waterfall_row_layout(
    const waterfall_row & row,
    nxt::rt::trace_clock::duration total,
    Rgba8 accent)
{
    namespace tt = tool_tui;
    auto children = std::vector<nxt::tui::AnyLayout>{};
    children.reserve(4);
    children.push_back(nxt::tui::fixed_width(
        28 * nxt::ch,
        nxt::tui::flex_text(
            row.name,
            nxt::tui::fg(tt::slate_300) | nxt::tui::bg(tt::page_bg))));
    children.push_back(nxt::tui::fixed_width(
        9 * nxt::ch,
        nxt::tui::text(
            std::format("+{:>7}", format_duration(row.offset)),
            nxt::tui::fg(tt::slate_500) | nxt::tui::bg(tt::page_bg))));
    children.push_back(nxt::tui::fixed_width(
        9 * nxt::ch,
        nxt::tui::text(
            std::format("{:>7}  ", format_duration(row.duration)),
            nxt::tui::fg(tt::slate_400) | nxt::tui::bg(tt::page_bg))));
    children.push_back(
        waterfall_bar(row.offset, row.duration, total, accent));
    return nxt::tui::row(std::move(children));
}

nxt::tui::AnyLayout render_waterfall(
    waterfall_view view,
    waterfall_options options)
{
    namespace tt = tool_tui;
    auto body = nxt::tui::AnyLayout{};
    if (view.rows.empty()) {
        body = tt::body_line("no completed child spans", tt::slate_500);
    } else {
        auto total = view.total;
        auto accent = options.accent;
        body = nxt::tui::each(
            std::move(view.rows),
            [total, accent](const waterfall_row & row) {
                return waterfall_row_layout(row, total, accent);
            });
    }

    return nxt::tui::surface(
        nxt::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxt::DEFAULT_EMPHASIS,
        },
        tt::block(waterfall_header(view, options), std::move(body)));
}

nxt::tui::AnyLayout render_span_waterfall(
    const nxt::rt::trace_context & trace,
    const nxt::rt::trace_span & span,
    waterfall_options options)
{
    auto subject = options.subject;
    auto view = collect_waterfall(trace, span, std::move(subject));
    return render_waterfall(std::move(view), std::move(options));
}

} // namespace nxt::llm::trace_tui
