#pragma once

#include <nxt/any_layout.hpp>
#include <nxt/tui.hpp>
#include <nxt/utf8.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::hud_blocks {

inline std::string truncate_cells(std::string_view text, nxt::width_t width)
{
    if (width.count() == 0)
        return {};
    if (nxt::tui::utf8_width(text) <= width)
        return std::string{text};
    if (width.count() <= 1)
        return "…";
    auto cut = nxt::utf8::byte_at_column(text, width - 1 * nxt::ch);
    return std::string{text.substr(0, cut.count())} + "…";
}

inline std::string pad_or_truncate(std::string_view text, nxt::width_t width)
{
    auto out = truncate_cells(text, width);
    auto used = nxt::tui::utf8_width(out);
    if (used < width)
        out += std::string(width.count() - used.count(), ' ');
    return out;
}

struct HeaderRow
{
    std::string marker;
    std::string label;
    std::string detail;
    std::string meta;
    nxt::Rgba8 accent{150, 190, 220};
    nxt::Rgba8 muted{125, 135, 150};
    nxt::tui::Style detail_style{};

    nxt::tui::WidthHint width_hint() const
    {
        return {24 * nxt::ch, 1.0 * nxt::one};
    }

    nxt::tui::HeightHint height_hint() const
    {
        return nxt::tui::HeightHint::fixed(1 * nxt::ln);
    }

    void render(nxt::RasterView & raster, nxt::Size size) const
    {
        std::ranges::fill(raster.glyphs(), 32);
        std::ranges::fill(raster.fgs(), nxt::DEFAULT_COLOR);
        std::ranges::fill(raster.bgs(), nxt::DEFAULT_COLOR);
        std::ranges::fill(raster.ems(), nxt::DEFAULT_EMPHASIS);

        auto meta_width = nxt::tui::utf8_width(meta);
        auto label_width = 12 * nxt::ch;
        auto left_reserved = 3 * nxt::ch + label_width;
        auto gap = meta.empty() ? 0 * nxt::ch : 1 * nxt::ch;
        auto detail_width =
            size.w > left_reserved + meta_width + gap
                ? size.w - left_reserved - meta_width - gap
                : 0 * nxt::ch;

        auto col = nxt::Pos::origin().x;
        col = nxt::tui::render_span(
            raster,
            {col, nxt::terminal_origin_v + 0 * nxt::ln},
            nxt::tui::span(marker, nxt::tui::fg(accent) | nxt::tui::bold));
        col = nxt::tui::render_span(
            raster,
            {col, nxt::terminal_origin_v + 0 * nxt::ln},
            nxt::tui::span(" ", {}));
        col = nxt::tui::render_span(
            raster,
            {col, nxt::terminal_origin_v + 0 * nxt::ln},
            nxt::tui::span(
                pad_or_truncate(label, label_width),
                nxt::tui::fg(accent) | nxt::tui::bold));
        nxt::tui::render_span(
            raster,
            {col, nxt::terminal_origin_v + 0 * nxt::ln},
            nxt::tui::span(truncate_cells(detail, detail_width), detail_style));

        if (!meta.empty() && size.w > meta_width) {
            auto meta_col = nxt::terminal_origin + (size.w - meta_width);
            nxt::tui::render_span(
                raster,
                {meta_col, nxt::terminal_origin_v + 0 * nxt::ln},
                nxt::tui::span(meta, nxt::tui::fg(muted)));
        }
    }
};

inline HeaderRow header_row(
    std::string marker,
    std::string label,
    std::string detail = {},
    std::string meta = {},
    nxt::Rgba8 accent = {150, 190, 220},
    nxt::tui::Style detail_style = nxt::tui::faint,
    nxt::Rgba8 muted = {125, 135, 150})
{
    return HeaderRow{
        std::move(marker),
        std::move(label),
        std::move(detail),
        std::move(meta),
        accent,
        muted,
        detail_style};
}

struct Column
{
    std::vector<nxt::tui::AnyLayout> rows;
    nxt::tui::AnyLayout active;

    nxt::tui::WidthHint width_hint() const
    {
        auto min = 0 * nxt::ch;
        for (const auto & row : rows)
            min = std::max(min, row.width_hint().min);
        min = std::max(min, active.width_hint().min);
        return {min, 1.0 * nxt::one};
    }

    nxt::tui::HeightHint height_hint() const
    {
        auto total = 0 * nxt::ln;
        for (const auto & row : rows)
            total = total + row.height_hint().min;
        total = total + active.height_hint().min;
        return nxt::tui::HeightHint::fixed(total);
    }

    void render(nxt::RasterView & raster, nxt::Size size) const
    {
        struct RowRef
        {
            const nxt::tui::AnyLayout * layout{};
            nxt::height_t height{0 * nxt::ln};
        };

        auto render_rows = std::vector<RowRef>{};
        render_rows.reserve(rows.size() + 1);
        for (const auto & row : rows) {
            auto h = row.height_hint().min;
            if (h.count() > 0)
                render_rows.push_back({&row, h});
        }
        auto active_h = active.height_hint().min;
        if (active_h.count() > 0)
            render_rows.push_back({&active, active_h});

        auto start = render_rows.size();
        auto visible_h = 0 * nxt::ln;
        while (start > 0) {
            auto next_h = render_rows[start - 1].height;
            if (visible_h + next_h > size.h) {
                if (visible_h == 0 * nxt::ln)
                    --start;
                break;
            }
            visible_h = visible_h + next_h;
            --start;
        }

        auto cursor = nxt::Pos::origin();
        auto render_one = [&](const RowRef & row) {
            auto h = row.height;
            if (h.count() == 0)
                return;
            auto used = cursor.y - nxt::Pos::origin().y;
            if (used >= size.h)
                return;
            auto child_h = std::min(h, size.h - used);
            auto child_size = nxt::Size{size.w, child_h};
            auto sub = nxt::tui::subraster(raster, cursor, child_size);
            row.layout->render(sub, child_size);
            cursor = cursor + child_h;
        };

        for (auto i = start; i < render_rows.size(); ++i)
            render_one(render_rows[i]);
    }
};

struct State
{
    std::vector<nxt::tui::AnyLayout> rows;

    template<nxt::tui::Layout L>
    void add(L && row)
    {
        rows.emplace_back(std::forward<L>(row));
    }

    [[nodiscard]] Column view(nxt::tui::AnyLayout active = {}) const
    {
        return Column{rows, std::move(active)};
    }

    template<nxt::tui::Layout L>
    [[nodiscard]] Column view(L && active) const
    {
        return view(nxt::tui::AnyLayout{std::forward<L>(active)});
    }
};

} // namespace nxt::ai::hud_blocks
