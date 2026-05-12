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

/// Byte offset into a UTF-8 string.
struct byte_offset_t
{
    /// Raw byte offset.
    std::size_t v{};

    /// Return the raw byte offset.
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

/// Grapheme-cluster index into a UTF-8 string.
struct grapheme_index_t
{
    /// Raw cluster index.
    std::size_t v{};

    /// Return the raw cluster index.
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

/// Construct a byte offset.
[[nodiscard]] constexpr byte_offset_t byte_offset(std::size_t n) noexcept
{
    return {n};
}

/// Construct a grapheme index.
[[nodiscard]] constexpr grapheme_index_t
grapheme_index(std::size_t n) noexcept
{
    return {n};
}

/// Difference between two byte offsets.
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

/// True when a grapheme cluster should separate words.
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

/// True when a grapheme cluster is a line break.
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

/// One non-separator word and its display width.
struct word
{
    /// View into the source text.
    std::string_view text;
    /// Display width in terminal cells.
    width_t width{};
};

/// Word or line-break segment used for streaming/wrapping text.
struct text_segment
{
    /// Segment category.
    enum class kind_t {
        word,
        line_break,
    };

    /// Category of this segment.
    kind_t kind = kind_t::word;
    /// View into the source text for word segments.
    std::string_view text;
    /// Display width in terminal cells.
    width_t width{};
};

/// Forward range over non-separator words in a string view.
class word_view : public std::ranges::view_interface<word_view>
{
public:
    /// Construct an empty view.
    word_view() = default;

    /// Construct a word view over borrowed text.
    explicit word_view(std::string_view text)
        : text_(text)
    {
    }

    /// Iterator over words.
    class iterator
    {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using value_type = word;
        using difference_type = std::ptrdiff_t;

        /// Construct a default sentinel-comparable iterator.
        iterator() = default;

        /// Construct an iterator at the first word in `text`.
        explicit iterator(std::string_view text)
            : text_(text)
        {
            scan();
        }

        /// Current word.
        [[nodiscard]] word operator*() const noexcept
        {
            return {
                .text = text_.substr(start_.count(), end_ - start_),
                .width = width_,
            };
        }

        /// Advance to the next word.
        iterator & operator++() noexcept
        {
            start_ = end_;
            scan();
            return *this;
        }

        /// Advance to the next word, returning the previous iterator.
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

    /// Begin iterating words.
    [[nodiscard]] iterator begin() const noexcept
    {
        return iterator{text_};
    }

    /// End sentinel.
    [[nodiscard]] std::default_sentinel_t end() const noexcept
    {
        return {};
    }

private:
    std::string_view text_;
};

/// Return a word range over borrowed text.
[[nodiscard]] inline word_view words(std::string_view text) noexcept
{
    return word_view{text};
}

/// Forward range over words and line breaks in a string view.
class segment_view : public std::ranges::view_interface<segment_view>
{
public:
    /// Construct an empty view.
    segment_view() = default;

    /// Construct a segment view over borrowed text.
    explicit segment_view(std::string_view text)
        : text_(text)
    {
    }

    /// Iterator over text segments.
    class iterator
    {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using value_type = text_segment;
        using difference_type = std::ptrdiff_t;

        /// Construct a default sentinel-comparable iterator.
        iterator() = default;

        /// Construct an iterator at the first segment in `text`.
        explicit iterator(std::string_view text)
            : text_(text)
        {
            scan();
        }

        /// Current segment.
        [[nodiscard]] text_segment operator*() const noexcept
        {
            return current_;
        }

        /// Advance to the next segment.
        iterator & operator++() noexcept
        {
            start_ = end_;
            scan();
            return *this;
        }

        /// Advance to the next segment, returning the previous iterator.
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
                        .text = {},
                        .width = 0 * ch,
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

    /// Begin iterating segments.
    [[nodiscard]] iterator begin() const noexcept
    {
        return iterator{text_};
    }

    /// End sentinel.
    [[nodiscard]] std::default_sentinel_t end() const noexcept
    {
        return {};
    }

private:
    std::string_view text_;
};

/// Return a segment range over borrowed text.
[[nodiscard]] inline segment_view segments(std::string_view text) noexcept
{
    return segment_view{text};
}

/// One paragraph split from borrowed text.
struct paragraph
{
    /// Paragraph contents, excluding surrounding blank-line separators.
    std::string_view text;
};

/// Forward range over paragraphs separated by blank lines.
class paragraph_view : public std::ranges::view_interface<paragraph_view>
{
public:
    /// Construct an empty view.
    paragraph_view() = default;

    /// Construct a paragraph view over borrowed text.
    explicit paragraph_view(std::string_view text)
        : text_(text)
    {}

    /// Iterator over paragraphs.
    class iterator
    {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using value_type = paragraph;
        using difference_type = std::ptrdiff_t;

        /// Construct a default sentinel-comparable iterator.
        iterator() = default;

        /// Construct an iterator at the first paragraph in `text`.
        explicit iterator(std::string_view text)
            : text_(text)
        {
            scan();
        }

        /// Current paragraph.
        [[nodiscard]] paragraph operator*() const noexcept
        {
            return {.text = text_.substr(start_.count(), end_ - start_)};
        }

        /// Advance to the next paragraph.
        iterator & operator++() noexcept
        {
            start_ = next_;
            scan();
            return *this;
        }

        /// Advance to the next paragraph, returning the previous iterator.
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
        struct line
        {
            byte_offset_t start{};
            byte_offset_t content_end{};
            byte_offset_t next{};
        };

        [[nodiscard]] line next_line(byte_offset_t pos) const noexcept
        {
            auto end = pos;
            while (end.count() < text_.size()) {
                auto n = next(text_, end);
                auto cluster = text_.substr(end.count(), n - end);
                if (is_line_break(cluster))
                    return {pos, end, n};
                end = n;
            }
            return {pos, end, end};
        }

        [[nodiscard]] bool blank(line l) const noexcept
        {
            auto pos = l.start;
            while (pos < l.content_end) {
                auto n = next(text_, pos);
                auto cluster = text_.substr(pos.count(), n - pos);
                if (!is_word_separator(cluster))
                    return false;
                pos = n;
            }
            return true;
        }

        void scan() noexcept
        {
            while (start_.count() < text_.size()) {
                auto l = next_line(start_);
                if (!blank(l))
                    break;
                start_ = l.next;
            }

            end_ = start_;
            next_ = start_;
            auto pos = start_;
            while (pos.count() < text_.size()) {
                auto l = next_line(pos);
                if (blank(l)) {
                    next_ = l.next;
                    return;
                }
                end_ = l.content_end;
                pos = l.next;
                next_ = pos;
            }
        }

        std::string_view text_;
        byte_offset_t start_{};
        byte_offset_t end_{};
        byte_offset_t next_{};
    };

    /// Begin iterating paragraphs.
    [[nodiscard]] iterator begin() const noexcept
    {
        return iterator{text_};
    }

    /// End sentinel.
    [[nodiscard]] std::default_sentinel_t end() const noexcept
    {
        return {};
    }

private:
    std::string_view text_;
};

/// Return a paragraph range over borrowed text.
[[nodiscard]] inline paragraph_view paragraphs(std::string_view text) noexcept
{
    return paragraph_view{text};
}

/// Size in bytes of the prefix that ends on a word boundary.
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
