#pragma once

#include "nxt/raster.hpp"
#include "nxt/utf8.hpp"

#include <algorithm>
#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace nxt::tui {

template<auto Unit>
struct hint_extent;

template<>
struct hint_extent<ch>
{
    using type = width_t;
};

template<>
struct hint_extent<ln>
{
    using type = height_t;
};

template<auto Unit>
using hint_extent_t = typename hint_extent<Unit>::type;

template<auto Unit>
struct SizeHint
{
    hint_extent_t<Unit> min{0 * Unit}; // Minimum size needed
    ratio_t flex{0.0 * one}; // Flex grow factor (0 = don't grow)

    static constexpr SizeHint fixed(hint_extent_t<Unit> n)
    {
        return {n, 0.0 * one};
    }

    static constexpr SizeHint grow(ratio_t factor = 1.0 * one)
    {
        return {0 * Unit, factor};
    }
};

using WidthHint = SizeHint<ch>;
using HeightHint = SizeHint<ln>;

template<typename L>
concept Layout =
    requires(const L & layout, RasterView & raster, Size size) {
        { layout.width_hint() } -> std::convertible_to<WidthHint>;
        { layout.height_hint() } -> std::convertible_to<HeightHint>;
        { layout.render(raster, size) } -> std::same_as<void>;
    };

template<typename RenderFn>
struct Leaf
{
    WidthHint w_hint;
    HeightHint h_hint;
    RenderFn render_fn;

    constexpr WidthHint width_hint() const
    {
        return w_hint;
    }

    constexpr HeightHint height_hint() const
    {
        return h_hint;
    }

    void render(RasterView & raster, Size size) const
    {
        render_fn(raster, size);
    }
};

template<typename F>
auto leaf(WidthHint w, HeightHint h, F && f)
{
    return Leaf<std::decay_t<F>>{w, h, std::forward<F>(f)};
}

template<Layout FalseLayout, Layout TrueLayout>
struct Either
{
    bool choose_true = false;
    FalseLayout false_layout;
    TrueLayout true_layout;

    constexpr WidthHint width_hint() const
    {
        return choose_true ? true_layout.width_hint()
                           : false_layout.width_hint();
    }

    constexpr HeightHint height_hint() const
    {
        return choose_true ? true_layout.height_hint()
                           : false_layout.height_hint();
    }

    void render(RasterView & raster, Size size) const
    {
        if (choose_true)
            true_layout.render(raster, size);
        else
            false_layout.render(raster, size);
    }
};

template<Layout FalseLayout, Layout TrueLayout>
auto either(
    bool choose_true,
    FalseLayout && false_layout,
    TrueLayout && true_layout)
{
    return Either<std::decay_t<FalseLayout>, std::decay_t<TrueLayout>>{
        choose_true,
        std::forward<FalseLayout>(false_layout),
        std::forward<TrueLayout>(true_layout),
    };
}

inline col_t write_text(RasterView & r, Pos pos, std::string_view text)
{
    return r.write_text(pos, text);
}

inline void set_fg(RasterView & r, Pos pos, Rgba8 color)
{
    r.set_fg(pos, color);
}

inline void set_bg(RasterView & r, Pos pos, Rgba8 color)
{
    r.set_bg(pos, color);
}

inline RasterView subraster(RasterView & r, Pos pos, Size size)
{
    return r.subraster(pos, size);
}

struct Style
{
    Rgba8 fg = DEFAULT_COLOR;
    Rgba8 bg = DEFAULT_COLOR;
    Emphasis em = DEFAULT_EMPHASIS;

    constexpr Style operator|(const Style & other) const
    {
        return {
            other.fg != DEFAULT_COLOR ? other.fg : fg,
            other.bg != DEFAULT_COLOR ? other.bg : bg,
            em | other.em,
        };
    }
};

constexpr Style fg(Rgba8 color)
{
    return {color, DEFAULT_COLOR, DEFAULT_EMPHASIS};
}

constexpr Style bg(Rgba8 color)
{
    return {DEFAULT_COLOR, color, DEFAULT_EMPHASIS};
}

constexpr Style em(Emphasis e)
{
    return {DEFAULT_COLOR, DEFAULT_COLOR, e};
}

inline constexpr Style bold{DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::bold};
inline constexpr Style faint{DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::faint};
inline constexpr Style italic{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::italic};
inline constexpr Style underline{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::underline};
inline constexpr Style reverse{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::reverse};
inline constexpr Style strikethrough{
    DEFAULT_COLOR, DEFAULT_COLOR, Emphasis::strikethrough};

struct Span
{
    std::string text;
    Style style{};
};

inline Span span(std::string text, Style s = {})
{
    return {std::move(text), s};
}

template<Layout Child>
struct Surface
{
    Style style{};
    Child child;

    constexpr WidthHint width_hint() const
    {
        return child.width_hint();
    }

    constexpr HeightHint height_hint() const
    {
        return child.height_hint();
    }

    void render(RasterView & raster, Size size) const
    {
        std::ranges::fill(raster.glyphs(), 32);
        std::ranges::fill(raster.fgs(), style.fg);
        std::ranges::fill(raster.bgs(), style.bg);
        std::ranges::fill(raster.ems(), style.em);
        child.render(raster, size);
    }
};

template<Layout Child>
auto surface(Style style, Child && child)
{
    return Surface<std::decay_t<Child>>{
        style,
        std::forward<Child>(child)};
}

template<Layout Child>
struct FixedHeight
{
    height_t height{0 * ln};
    Child child;

    constexpr WidthHint width_hint() const
    {
        return child.width_hint();
    }

    constexpr HeightHint height_hint() const
    {
        return HeightHint::fixed(height);
    }

    void render(RasterView & raster, Size size) const
    {
        child.render(raster, size);
    }
};

template<Layout Child>
auto fixed_height(height_t height, Child && child)
{
    return FixedHeight<std::decay_t<Child>>{
        height,
        std::forward<Child>(child)};
}

inline col_t render_span(RasterView & r, Pos pos, const Span & s)
{
    const auto start_x = pos.x;
    const auto end_x = r.write_text(pos, s.text);

    for (auto x = start_x; x < end_x; x += 1 * ch) {
        const Pos p{x, pos.y};
        if (s.style.fg != DEFAULT_COLOR)
            r.set_fg(p, s.style.fg);
        if (s.style.bg != DEFAULT_COLOR)
            r.set_bg(p, s.style.bg);
        if (s.style.em != DEFAULT_EMPHASIS)
            r.set_em(p, s.style.em);
    }

    return end_x;
}

inline std::string repeat(std::string_view glyph, width_t w)
{
    auto n = w.count();
    std::string result;
    result.reserve(glyph.size() * n);
    for (std::size_t i = 0; i < n; ++i)
        result += glyph;
    return result;
}

inline width_t utf8_width(std::string_view s)
{
    return utf8::display_width(s);
}

inline auto text(std::string s)
{
    auto w = utf8_width(s);
    return leaf(
        WidthHint::fixed(w),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size) {
            std::ranges::fill(r.fgs(), DEFAULT_COLOR);
            std::ranges::fill(r.bgs(), DEFAULT_COLOR);
            std::ranges::fill(r.ems(), DEFAULT_EMPHASIS);
            std::ranges::fill(r.glyphs(), 32);
            render_span(r, Pos::origin(), Span{s, {}});
        });
}

inline auto text(std::string s, Style style)
{
    auto w = utf8_width(s);
    return leaf(
        WidthHint::fixed(w),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size) {
            std::ranges::fill(r.fgs(), style.fg);
            std::ranges::fill(r.bgs(), style.bg);
            std::ranges::fill(r.ems(), style.em);
            std::ranges::fill(r.glyphs(), 32);
            render_span(r, Pos::origin(), Span{s, style});
        });
}

template<typename... Spans>
    requires(std::same_as<std::decay_t<Spans>, Span> && ...)
inline auto styled_text(Spans &&... spans)
{
    width_t total_w = 0 * ch;
    ((total_w += utf8_width(spans.text)), ...);

    auto span_tuple = std::tuple{std::forward<Spans>(spans)...};

    return leaf(
        WidthHint::fixed(total_w),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size) {
            col_t col = Pos::origin().x;
            std::apply(
                [&](const auto &... s) {
                    ((col = render_span(r, Pos{col, Pos::origin().y}, s)),
                     ...);
                },
                span_tuple);
        });
}

inline auto fill(Rgba8 color = Rgba8(60, 60, 60))
{
    return leaf(
        WidthHint::grow(), HeightHint::grow(), [=](RasterView & r, Size) {
            std::ranges::fill(r.bgs(), color);
        });
}

inline std::string hrule_string(width_t w)
{
    return repeat("─", w);
}

inline auto hrule()
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size size) {
            write_text(r, Pos::origin(), hrule_string(size.w));
        });
}

inline std::string bar_string(percent_t pct, width_t width)
{
    static constexpr std::array<std::string_view, 9> partials = {
        "", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};

    auto w = width.count();
    auto fraction =
        std::clamp(pct.value(), 0.0, 100.0) / 100.0;
    double fill = fraction * w;
    std::size_t full = static_cast<std::size_t>(fill);
    std::size_t partial = static_cast<std::size_t>((fill - full) * 8 + 0.5);

    if (partial >= 8) {
        full++;
        partial = 0;
    }

    return repeat("█", std::min(full, w) * ch)
           + std::string(partials[partial]);
}

inline auto progress_bar(
    percent_t pct,
    Rgba8 fg = Rgba8(100, 180, 255),
    Rgba8 bg = Rgba8(50, 50, 50))
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size) {
            std::ranges::fill(r.bgs(), bg);
            std::ranges::fill(r.fgs(), fg);
            r.write_text(Pos::origin(), bar_string(pct, r.width()));
        });
}

template<Layout... Children>
struct Row
{
    std::tuple<Children...> children;

    constexpr WidthHint width_hint() const
    {
        width_t total_min = 0 * ch;
        ratio_t total_flex = 0.0 * one;
        std::apply(
            [&](const auto &... c) {
                ((total_min += c.width_hint().min,
                  total_flex += c.width_hint().flex),
                 ...);
            },
            children);
        return {total_min, total_flex};
    }

    constexpr HeightHint height_hint() const
    {
        height_t max_min = 0 * ln;
        std::apply(
            [&](const auto &... c) {
                ((max_min = std::max(max_min, c.height_hint().min)), ...);
            },
            children);
        return HeightHint::fixed(
            max_min.count() > 0 ? max_min
                                               : height_t{1 * ln});
    }

    void render(RasterView & raster, Size size) const
    {
        constexpr std::size_t N = sizeof...(Children);
        if constexpr (N == 0)
            return;

        std::array<WidthHint, N> hints;
        std::size_t i = 0;
        std::apply(
            [&](const auto &... c) {
                ((hints[i++] = c.width_hint()), ...);
            },
            children);

        auto widths = flex_distribute(size.w, hints);

        Pos cursor = Pos::origin();
        i = 0;
        std::apply(
            [&](const auto &... c) {
                (
                    [&] {
                        auto child_size = Size{widths[i], size.h};
                        if (widths[i].count() > 0) {
                            auto sub =
                                subraster(raster, cursor, child_size);
                            c.render(sub, child_size);
                            cursor += widths[i];
                        }
                        ++i;
                    }(),
                    ...);
            },
            children);
    }

private:
    template<std::size_t N>
    static std::array<width_t, N>
    flex_distribute(width_t total, const std::array<WidthHint, N> & hints)
    {
        std::array<width_t, N> result{};

        width_t used = 0 * ch;
        ratio_t total_flex = 0.0 * one;
        for (std::size_t i = 0; i < N; ++i) {
            result[i] = hints[i].min;
            used += hints[i].min;
            total_flex += hints[i].flex;
        }

        if (total_flex > 0 && total > used) {
            auto remaining = total - used;
            for (std::size_t i = 0; i < N; ++i) {
                auto flex_val = hints[i].flex;
                auto total_flex_val = total_flex;
                if (flex_val > 0)
                    result[i] += remaining
                                 * (flex_val.value()
                                    / total_flex_val.value());
            }
        }

        return result;
    }
};

template<Layout... Children>
Row<std::decay_t<Children>...> row(Children &&... children)
{
    return {std::tuple{std::forward<Children>(children)...}};
}

template<Layout... Children>
struct Column
{
    std::tuple<Children...> children;

    constexpr WidthHint width_hint() const
    {
        width_t max_min = 0 * ch;
        std::apply(
            [&](const auto &... c) {
                ((max_min = std::max(max_min, c.width_hint().min)), ...);
            },
            children);
        return {max_min, 1.0 * one};
    }

    constexpr HeightHint height_hint() const
    {
        height_t total_min = 0 * ln;
        ratio_t total_flex = 0.0 * one;
        std::apply(
            [&](const auto &... c) {
                ((total_min += c.height_hint().min,
                  total_flex += c.height_hint().flex),
                 ...);
            },
            children);
        return {total_min, total_flex};
    }

    void render(RasterView & raster, Size size) const
    {
        constexpr std::size_t N = sizeof...(Children);
        if constexpr (N == 0)
            return;

        std::array<HeightHint, N> hints;
        std::size_t i = 0;
        std::apply(
            [&](const auto &... c) {
                ((hints[i++] = c.height_hint()), ...);
            },
            children);

        auto heights = flex_distribute(size.h, hints);

        Pos cursor = Pos::origin();
        i = 0;
        std::apply(
            [&](const auto &... c) {
                (
                    [&] {
                        auto child_size = Size{size.w, heights[i]};
                        if (heights[i].count() > 0) {
                            auto sub =
                                subraster(raster, cursor, child_size);
                            c.render(sub, child_size);
                            cursor = cursor
                                     + heights[i]; // point + vector = point
                        }
                        ++i;
                    }(),
                    ...);
            },
            children);
    }

private:
    template<std::size_t N>
    static std::array<height_t, N>
    flex_distribute(height_t total, const std::array<HeightHint, N> & hints)
    {
        std::array<height_t, N> result{};

        height_t used = 0 * ln;
        ratio_t total_flex = 0.0 * one;
        for (std::size_t i = 0; i < N; ++i) {
            result[i] = hints[i].min;
            used += hints[i].min;
            total_flex += hints[i].flex;
        }

        if (total_flex > 0 && total > used) {
            auto remaining = total - used;
            for (std::size_t i = 0; i < N; ++i) {
                auto flex_val = hints[i].flex;
                auto total_flex_val = total_flex;
                if (flex_val > 0)
                    result[i] += remaining
                                 * (flex_val.value()
                                    / total_flex_val.value());
            }
        }

        return result;
    }
};

template<Layout... Children>
Column<std::decay_t<Children>...> column(Children &&... children)
{
    return {std::tuple{std::forward<Children>(children)...}};
}

template<typename T, typename ViewFn>
struct List
{
    std::span<const T> items;
    ViewFn view;

    constexpr WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    constexpr HeightHint height_hint() const
    {
        return HeightHint::fixed(items.size() * ln);
    }

    void render(RasterView & raster, Size size) const
    {
        const auto row_size = Size{size.w, 1 * ln};

        Pos cursor = Pos::origin();

        for (const auto & item : items) {
            if (cursor.y - Pos::origin().y >= size.h)
                break;

            auto child = view(item);
            auto sub = subraster(raster, cursor, row_size);

            child.render(sub, row_size);
            cursor += 1 * ln;
        }
    }
};

template<typename T, typename ViewFn>
List<T, ViewFn> list(std::span<const T> items, ViewFn && view)
{
    return {items, std::forward<ViewFn>(view)};
}

template<typename T, typename ViewFn>
auto list(const std::vector<T> & items, ViewFn && view)
{
    return list(std::span<const T>(items), std::forward<ViewFn>(view));
}

} // namespace nxt::tui
