// Small UTF-8 helpers for terminal-cell text.
//
// Movement is by extended grapheme cluster, and display width is derived
// from utf8proc character widths. Malformed bytes are treated as one
// replacement-width cluster each.

#pragma once

#include "nxt/units.hpp"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

#include <utf8proc.h>

namespace nxt::utf8 {

struct byte_offset_t
{
    std::size_t v{};

    [[nodiscard]] constexpr std::size_t count() const noexcept
    {
        return v;
    }

    constexpr byte_offset_t & operator+=(std::size_t bytes) noexcept
    {
        v += bytes;
        return *this;
    }

    friend constexpr bool operator==(
        byte_offset_t, byte_offset_t) noexcept = default;
    friend constexpr auto operator<=>(
        byte_offset_t, byte_offset_t) noexcept = default;
};

struct grapheme_index_t
{
    std::size_t v{};

    [[nodiscard]] constexpr std::size_t count() const noexcept
    {
        return v;
    }

    constexpr grapheme_index_t & operator+=(std::size_t clusters) noexcept
    {
        v += clusters;
        return *this;
    }

    friend constexpr bool operator==(
        grapheme_index_t, grapheme_index_t) noexcept = default;
    friend constexpr auto operator<=>(
        grapheme_index_t, grapheme_index_t) noexcept = default;
};

[[nodiscard]] constexpr byte_offset_t byte_offset(std::size_t n) noexcept
{
    return {n};
}

[[nodiscard]] constexpr grapheme_index_t
grapheme_index(std::size_t n) noexcept
{
    return {n};
}

[[nodiscard]] constexpr std::size_t
operator-(byte_offset_t a, byte_offset_t b) noexcept
{
    return a.v - b.v;
}

namespace detail {

struct Decoded
{
    std::size_t length = 1;
    utf8proc_int32_t codepoint = 0xfffd;
    bool valid = false;
};

[[nodiscard]] inline Decoded
decode(std::string_view text, byte_offset_t byte) noexcept
{
    if (byte.count() >= text.size())
        return {.length = 0, .codepoint = 0, .valid = false};

    utf8proc_int32_t cp = 0;
    const auto remaining =
        static_cast<utf8proc_ssize_t>(text.size() - byte.count());
    const auto len = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(
            text.data() + byte.count()),
        remaining,
        &cp);
    if (len < 0)
        return {};

    return {
        .length = static_cast<std::size_t>(len),
        .codepoint = cp,
        .valid = true,
    };
}

[[nodiscard]] inline bool regional_indicator(utf8proc_int32_t cp) noexcept
{
    return cp >= 0x1f1e6 && cp <= 0x1f1ff;
}

}  // namespace detail

[[nodiscard]] inline bool is_word_separator(
    std::string_view cluster) noexcept
{
    auto byte = byte_offset(0);
    while (byte.count() < cluster.size()) {
        const auto d = detail::decode(cluster, byte);
        if (!d.valid)
            return false;

        switch (d.codepoint) {
        case '\t':
        case '\n':
        case '\v':
        case '\f':
        case '\r':
            return true;
        default:
            break;
        }

        switch (utf8proc_category(d.codepoint)) {
        case UTF8PROC_CATEGORY_ZS:
        case UTF8PROC_CATEGORY_ZL:
        case UTF8PROC_CATEGORY_ZP:
            return true;
        default:
            break;
        }

        byte += d.length;
    }

    return false;
}

[[nodiscard]] inline bool is_line_break(std::string_view cluster) noexcept
{
    return cluster == "\n" || cluster == "\r\n" || cluster == "\r";
}

/// Byte offset of the grapheme cluster after the one starting at `byte`.
/// Returns `text.size()` if `byte` is at or past the end.
[[nodiscard]] inline byte_offset_t
next(std::string_view text, byte_offset_t byte) noexcept
{
    if (byte.count() >= text.size())
        return byte_offset(text.size());

    auto current = detail::decode(text, byte);
    if (!current.valid)
        return byte_offset(byte.count() + current.length);

    auto end = byte_offset(byte.count() + current.length);
    utf8proc_int32_t state = 0;
    while (end.count() < text.size()) {
        auto following = detail::decode(text, end);
        if (!following.valid)
            break;
        if (utf8proc_grapheme_break_stateful(
                current.codepoint, following.codepoint, &state))
            break;
        current = following;
        end += following.length;
    }

    return end;
}

/// Byte offset of the grapheme cluster before the one ending at `byte`.
/// Returns 0 if `byte` is at the start.
[[nodiscard]] inline byte_offset_t
prev(std::string_view text, byte_offset_t byte) noexcept
{
    if (byte.count() > text.size())
        byte = byte_offset(text.size());
    if (byte.count() == 0)
        return byte_offset(0);

    for (auto b = byte_offset(0); b.count() < text.size();) {
        const auto n = next(text, b);
        if (n >= byte)
            return b;
        b = n;
    }

    return byte_offset(text.size());
}

/// Byte offset of the nearest grapheme boundary at or before `byte`.
[[nodiscard]] inline byte_offset_t
floor_boundary(std::string_view text, byte_offset_t byte) noexcept
{
    if (byte.count() >= text.size())
        return byte_offset(text.size());

    auto boundary = byte_offset(0);
    for (auto b = byte_offset(0); b.count() < text.size();) {
        const auto n = next(text, b);
        if (byte < n)
            return boundary;
        boundary = n;
        b = n;
    }

    return byte_offset(text.size());
}

/// Number of grapheme clusters in `text`.
[[nodiscard]] inline grapheme_index_t count(std::string_view text) noexcept
{
    std::size_t n = 0;
    for (auto b = byte_offset(0); b.count() < text.size(); b = next(text, b))
        ++n;
    return grapheme_index(n);
}

/// Display width of one grapheme cluster.
[[nodiscard]] inline width_t cluster_width(
    std::string_view cluster) noexcept
{
    auto byte = byte_offset(0);
    auto sum = 0 * ch;
    auto max_width = 0 * ch;
    std::size_t regional_indicators = 0;
    bool only_regional_indicators = !cluster.empty();
    bool has_zwj = false;

    while (byte.count() < cluster.size()) {
        const auto d = detail::decode(cluster, byte);
        if (!d.valid)
            return 1 * ch;

        if (d.codepoint == 0x200d)
            has_zwj = true;

        const bool is_ri = detail::regional_indicator(d.codepoint);
        if (is_ri)
            ++regional_indicators;
        else
            only_regional_indicators = false;

        const auto width = utf8proc_charwidth(d.codepoint);
        if (width > 0) {
            const auto w = static_cast<std::size_t>(width) * ch;
            sum += w;
            if (w > max_width)
                max_width = w;
        }

        byte += d.length;
    }

    if (has_zwj)
        return max_width == 0 * ch ? 1 * ch : max_width;
    if (only_regional_indicators && regional_indicators >= 2)
        return 2 * ch;
    return sum == 0 * ch ? 1 * ch : sum;
}

/// Display width of all grapheme clusters in `text`.
[[nodiscard]] inline width_t display_width(
    std::string_view text) noexcept
{
    auto width = 0 * ch;
    for (auto b = byte_offset(0); b.count() < text.size();) {
        const auto n = next(text, b);
        width += cluster_width(text.substr(b.count(), n - b));
        b = n;
    }
    return width;
}

struct word
{
    std::string_view text;
    width_t width{};
};

struct text_segment
{
    enum class kind_t {
        word,
        line_break,
    };

    kind_t kind = kind_t::word;
    std::string_view text;
    width_t width{};
};

class word_view : public std::ranges::view_interface<word_view>
{
public:
    word_view() = default;

    explicit word_view(std::string_view text)
        : text_(text)
    {
    }

    class iterator
    {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using value_type = word;
        using difference_type = std::ptrdiff_t;

        iterator() = default;

        explicit iterator(std::string_view text)
            : text_(text)
        {
            scan();
        }

        [[nodiscard]] word operator*() const noexcept
        {
            return {
                .text = text_.substr(start_.count(), end_ - start_),
                .width = width_,
            };
        }

        iterator & operator++() noexcept
        {
            start_ = end_;
            scan();
            return *this;
        }

        iterator operator++(int) noexcept
        {
            auto copy = *this;
            ++*this;
            return copy;
        }

        friend bool operator==(const iterator & it, std::default_sentinel_t)
            noexcept
        {
            return it.start_.count() >= it.text_.size();
        }

    private:
        void scan() noexcept
        {
            while (start_.count() < text_.size()) {
                auto next_byte = next(text_, start_);
                auto cluster =
                    text_.substr(start_.count(), next_byte - start_);
                if (!is_word_separator(cluster))
                    break;
                start_ = next_byte;
            }

            end_ = start_;
            width_ = 0 * ch;
            while (end_.count() < text_.size()) {
                auto next_byte = next(text_, end_);
                auto cluster = text_.substr(end_.count(), next_byte - end_);
                if (is_word_separator(cluster))
                    break;
                width_ += cluster_width(cluster);
                end_ = next_byte;
            }
        }

        std::string_view text_;
        byte_offset_t start_{};
        byte_offset_t end_{};
        width_t width_{};
    };

    [[nodiscard]] iterator begin() const noexcept
    {
        return iterator{text_};
    }

    [[nodiscard]] std::default_sentinel_t end() const noexcept
    {
        return {};
    }

private:
    std::string_view text_;
};

[[nodiscard]] inline word_view words(std::string_view text) noexcept
{
    return word_view{text};
}

class segment_view : public std::ranges::view_interface<segment_view>
{
public:
    segment_view() = default;

    explicit segment_view(std::string_view text)
        : text_(text)
    {
    }

    class iterator
    {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using value_type = text_segment;
        using difference_type = std::ptrdiff_t;

        iterator() = default;

        explicit iterator(std::string_view text)
            : text_(text)
        {
            scan();
        }

        [[nodiscard]] text_segment operator*() const noexcept
        {
            return current_;
        }

        iterator & operator++() noexcept
        {
            start_ = end_;
            scan();
            return *this;
        }

        iterator operator++(int) noexcept
        {
            auto copy = *this;
            ++*this;
            return copy;
        }

        friend bool operator==(const iterator & it, std::default_sentinel_t)
            noexcept
        {
            return it.start_.count() >= it.text_.size();
        }

    private:
        void scan() noexcept
        {
            while (start_.count() < text_.size()) {
                auto next_byte = next(text_, start_);
                auto cluster =
                    text_.substr(start_.count(), next_byte - start_);
                if (is_line_break(cluster)) {
                    end_ = next_byte;
                    current_ = text_segment{
                        .kind = text_segment::kind_t::line_break,
                    };
                    return;
                }
                if (!is_word_separator(cluster))
                    break;
                start_ = next_byte;
            }

            end_ = start_;
            auto width = 0 * ch;
            while (end_.count() < text_.size()) {
                auto next_byte = next(text_, end_);
                auto cluster = text_.substr(end_.count(), next_byte - end_);
                if (is_word_separator(cluster))
                    break;
                width += cluster_width(cluster);
                end_ = next_byte;
            }

            current_ = text_segment{
                .text = text_.substr(start_.count(), end_ - start_),
                .width = width,
            };
        }

        std::string_view text_;
        byte_offset_t start_{};
        byte_offset_t end_{};
        text_segment current_{};
    };

    [[nodiscard]] iterator begin() const noexcept
    {
        return iterator{text_};
    }

    [[nodiscard]] std::default_sentinel_t end() const noexcept
    {
        return {};
    }

private:
    std::string_view text_;
};

[[nodiscard]] inline segment_view segments(std::string_view text) noexcept
{
    return segment_view{text};
}

[[nodiscard]] inline std::size_t
complete_words_prefix_size(std::string_view text) noexcept
{
    auto consumed = std::size_t{0};
    for (auto b = byte_offset(0); b.count() < text.size();) {
        auto n = next(text, b);
        auto cluster = text.substr(b.count(), n - b);
        if (is_word_separator(cluster))
            consumed = n.count();
        b = n;
    }
    return consumed;
}

/// Byte offset at the `cell`-th grapheme cluster (clamped to text.size()).
[[nodiscard]] inline byte_offset_t
byte_at(std::string_view text, grapheme_index_t cell) noexcept
{
    auto b = byte_offset(0);
    for (std::size_t i = 0; i < cell.count() && b.count() < text.size(); ++i)
        b = next(text, b);
    return b;
}

/// Grapheme cluster index for a byte offset, snapped to the previous boundary.
[[nodiscard]] inline grapheme_index_t
cell_at(std::string_view text, byte_offset_t byte) noexcept
{
    byte = floor_boundary(text, byte);
    std::size_t i = 0;
    for (auto b = byte_offset(0); b < byte; b = next(text, b))
        ++i;
    return grapheme_index(i);
}

/// Display column for a byte offset, snapped to the previous boundary.
[[nodiscard]] inline width_t
column_at(std::string_view text, byte_offset_t byte) noexcept
{
    byte = floor_boundary(text, byte);
    auto col = 0 * ch;
    for (auto b = byte_offset(0); b < byte;) {
        const auto n = next(text, b);
        col += cluster_width(text.substr(b.count(), n - b));
        b = n;
    }
    return col;
}

/// Byte offset for the grapheme cluster at `column`.
[[nodiscard]] inline byte_offset_t
byte_at_column(std::string_view text, width_t column) noexcept
{
    auto col = 0 * ch;
    for (auto b = byte_offset(0); b.count() < text.size();) {
        const auto n = next(text, b);
        const auto w = cluster_width(text.substr(b.count(), n - b));
        if (col + w > column)
            return b;
        col += w;
        if (col >= column)
            return n;
        b = n;
    }
    return byte_offset(text.size());
}

}  // namespace nxt::utf8
