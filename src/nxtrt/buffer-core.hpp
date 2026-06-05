#pragma once

#include "nxtrt/task.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <expected>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace nxtrt {

struct buffer_error : runtime_error
{
    using runtime_error::runtime_error;
};

struct end_of_stream : buffer_error
{
    using buffer_error::buffer_error;
};

/// View immutable bytes as text without copying.
inline std::string_view
as_string_view(std::span<const std::byte> bytes) noexcept
{
    return {
        reinterpret_cast<const char *>(bytes.data()),
        bytes.size_bytes(),
    };
}

inline std::span<const std::byte> as_bytes(std::string_view text) noexcept
{
    return std::as_bytes(std::span{text});
}

inline std::size_t find_bytes(
    std::span<const std::byte> haystack, std::span<const std::byte> needle)
{
    auto match = std::ranges::search(haystack, needle);
    return static_cast<std::size_t>(
        std::distance(haystack.begin(), match.begin()));
}

struct eof_t
{};

inline constexpr auto eof = eof_t{};

/// Values accepted by the requested destination, or explicit EOF.
///
/// A successful zero count is valid progresslessness; EOF is represented by
/// the error alternative instead of by a count value.
class fare_t
{
public:
    fare_t(std::size_t n = 0) noexcept
        : result_(n)
    {
    }

    fare_t(eof_t) noexcept
        : result_(std::unexpected{eof})
    {
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return result_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return result_.has_value();
    }

    [[nodiscard]] std::size_t operator*() const noexcept
    {
        return *result_;
    }

private:
    std::expected<std::size_t, eof_t> result_;
};

inline std::size_t value_count(fare_t const & result) noexcept
{
    return result ? *result : 0;
}

inline bool is_eof(fare_t const & result) noexcept
{
    return !result;
}

template<typename T, std::size_t Inline = 2>
class buffer_chunks
{
public:
    using value_type = T;
    using chunk_type = std::span<T>;

    buffer_chunks() = default;

    buffer_chunks(chunk_type chunk)
    {
        append(chunk);
    }

    buffer_chunks(std::span<const chunk_type> chunks)
    {
        for (auto chunk : chunks)
            append(chunk);
    }

    [[nodiscard]] std::span<const chunk_type> chunks() const noexcept
    {
        return std::span{chunks_.data(), count_};
    }

    [[nodiscard]] auto begin() const noexcept
    {
        return chunks().begin();
    }

    [[nodiscard]] auto end() const noexcept
    {
        return chunks().end();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size() == 0;
    }

    [[nodiscard]] std::size_t chunk_count() const noexcept
    {
        return count_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        auto total = std::size_t{0};
        for (auto chunk : chunks())
            total += chunk.size();
        return total;
    }

    [[nodiscard]] buffer_chunks first(std::size_t n) const
    {
        auto out = buffer_chunks{};
        for (auto chunk : chunks()) {
            if (n == 0)
                break;
            auto take = std::min(n, chunk.size());
            out.append(chunk.first(take));
            n -= take;
        }
        return out;
    }

    [[nodiscard]] buffer_chunks subspan(
        std::size_t offset,
        std::size_t count = std::numeric_limits<std::size_t>::max()) const
    {
        auto out = buffer_chunks{};
        for (auto chunk : chunks()) {
            if (offset >= chunk.size()) {
                offset -= chunk.size();
                continue;
            }

            auto rest = chunk.subspan(offset);
            offset = 0;
            auto take = std::min(count, rest.size());
            out.append(rest.first(take));
            count -= take;
            if (count == 0)
                break;
        }
        return out;
    }

    [[nodiscard]] std::optional<chunk_type> single_span() const noexcept
    {
        if (count_ == 0)
            return chunk_type{};
        if (count_ == 1)
            return chunks_[0];
        return std::nullopt;
    }

private:
    void append(chunk_type chunk)
    {
        if (chunk.empty())
            return;
        if (count_ == Inline)
            throw buffer_error{"too many buffer chunks"};
        chunks_[count_++] = chunk;
    }

    std::array<chunk_type, Inline> chunks_{};
    std::size_t count_ = 0;
};

template<typename T = std::byte, std::size_t Inline = 2>
using byte_chunks = buffer_chunks<T, Inline>;

/// View of raw storage where up to `size()` values of `T` may be
/// constructed.
///
/// Unlike `std::span<T>`, this does not claim that live `T` objects already
/// exist. Producers must start object lifetimes before reporting values as
/// constructed to a ring, feed, or sink.
template<typename T>
class junk
{
public:
    using value_type = std::remove_cv_t<T>;

    junk() = default;

    junk(value_type * data, std::size_t size)
        : data_(data)
        , size_(size)
    {
    }

    [[nodiscard]] value_type * data() const noexcept
    {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] junk first(std::size_t n) const noexcept
    {
        return {data_, std::min(n, size_)};
    }

    [[nodiscard]] std::span<std::byte> as_writable_bytes() const noexcept
        requires std::is_trivially_copyable_v<value_type>
    {
        return {
            reinterpret_cast<std::byte *>(data_),
            size_ * sizeof(value_type),
        };
    }

private:
    value_type * data_ = nullptr;
    std::size_t size_ = 0;
};

/// Scanner response for a frame whose complete source stock is not visible
/// yet.
///
/// `minimum_buffered` is relative to the chunk view passed to the scanner.
/// A length-prefixed scanner can ask for the whole next frame immediately,
/// while a delimiter scanner can ask for one more visible source value.
struct chop_need_more
{
    std::size_t minimum_buffered = 0;
};

/// One visible frame projection and the source prefix it occupies.
///
/// This is the `(extent, frame)` pair described in
/// @ref rfc_reels_chop "RFC 0001: Reels / Chop". The frame borrows from the
/// chunks used to construct it; the extent is what lets the next scan start
/// at the following frame boundary.
template<typename Frame>
struct frame_chop
{
    std::size_t extent = 0;
    Frame frame;
};

template<typename Frame>
using chop_scan_result = std::variant<frame_chop<Frame>, chop_need_more>;

template<typename Scanner, typename Stock, typename Frame>
concept chop_scanner =
    requires(const Scanner & scanner, buffer_chunks<const Stock> stock) {
        {
            std::invoke(scanner, stock)
        } -> std::same_as<chop_scan_result<Frame>>;
    };

template<typename Stock, typename Frame>
struct static_chop_scanner
{
    chop_scan_result<Frame>
    operator()(buffer_chunks<const Stock> stock) const
    {
        return Frame::scan(stock);
    }
};

/// Stateless lopping view over visible source stock.
///
/// `chop_view` is the pure range part of @ref rfc_reels_chop
/// "RFC 0001: Reels / Chop". It stores only the current source chunks and a
/// scanner, then recomputes frame boundaries as iteration advances. It does
/// not own storage, cache marks, or model frames as queued values.
template<
    typename Stock,
    typename Frame,
    chop_scanner<Stock, Frame> Scanner = static_chop_scanner<Stock, Frame>>
class chop_view
    : public std::ranges::view_interface<chop_view<Stock, Frame, Scanner>>
{
public:
    using stock_type = Stock;
    using value_type = frame_chop<Frame>;

    chop_view() = default;

    chop_view(buffer_chunks<const Stock> stock, Scanner scanner = {})
        : stock_(stock)
        , scanner_(std::move(scanner))
    {
    }

    class iterator
    {
    public:
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = frame_chop<Frame>;
        using difference_type = std::ptrdiff_t;

        iterator() = default;

        iterator(const chop_view * parent, std::size_t offset)
            : parent_(parent)
            , offset_(offset)
        {
            refresh();
        }

        [[nodiscard]] const value_type & operator*() const noexcept
        {
            return *current_;
        }

        [[nodiscard]] const value_type * operator->() const noexcept
        {
            return &*current_;
        }

        iterator & operator++()
        {
            offset_ += current_->extent;
            refresh();
            return *this;
        }

        void operator++(int)
        {
            (void) ++*this;
        }

        [[nodiscard]] friend bool
        operator==(const iterator & it, std::default_sentinel_t) noexcept
        {
            return !it.current_.has_value();
        }

    private:
        void refresh()
        {
            current_.reset();
            if (parent_ == nullptr)
                return;

            auto suffix = parent_->stock_.subspan(offset_);
            if (suffix.empty())
                return;

            auto scan = std::invoke(parent_->scanner_, suffix);
            auto * complete = std::get_if<value_type>(&scan);
            if (complete == nullptr)
                return;

            if (complete->extent == 0)
                throw buffer_error{"chop scanner returned zero extent"};
            if (complete->extent > suffix.size())
                throw buffer_error{"chop scanner overreported extent"};
            current_ = std::move(*complete);
        }

        const chop_view * parent_ = nullptr;
        std::size_t offset_ = 0;
        std::optional<value_type> current_;
    };

    [[nodiscard]] iterator begin() const
    {
        return iterator{this, 0};
    }

    [[nodiscard]] std::default_sentinel_t end() const noexcept
    {
        return {};
    }

    [[nodiscard]] bool empty() const
    {
        return begin() == end();
    }

    [[nodiscard]] std::size_t count() const
    {
        auto n = std::size_t{0};
        for (auto const & ignored : *this) {
            (void) ignored;
            ++n;
        }
        return n;
    }

    [[nodiscard]] bool has_at_least(std::size_t n) const
    {
        if (n == 0)
            return true;
        for (auto const & ignored : *this) {
            (void) ignored;
            if (--n == 0)
                return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t
    extent(std::size_t max_count = std::numeric_limits<std::size_t>::max())
        const
    {
        auto out = std::size_t{0};
        for (auto const & item : *this) {
            if (max_count == 0)
                break;
            out += item.extent;
            --max_count;
        }
        return out;
    }

private:
    buffer_chunks<const Stock> stock_;
    Scanner scanner_{};
};

template<
    typename Stock,
    typename Frame,
    typename Scanner = static_chop_scanner<Stock, Frame>>
    requires chop_scanner<Scanner, Stock, Frame>
[[nodiscard]] chop_view<Stock, Frame, Scanner>
chop(buffer_chunks<const Stock> stock, Scanner scanner = {})
{
    return {stock, std::move(scanner)};
}

template<std::ranges::input_range Chops>
[[nodiscard]] std::size_t chop_extent(Chops && chops)
{
    auto out = std::size_t{0};
    for (auto && item : chops)
        out += item.extent;
    return out;
}

namespace detail {

template<typename T>
[[nodiscard]] buffer_chunks<T> ring_chunks(
    T * data,
    std::size_t capacity,
    std::size_t seek,
    std::size_t size) noexcept
{
    if (size == 0)
        return {};

    auto first = std::min(size, capacity - seek);
    if (first == size)
        return buffer_chunks<T>{std::span<T>{data + seek, first}};

    auto chunks = std::array{
        std::span<T>{data + seek, first},
        std::span<T>{data, size - first},
    };
    return buffer_chunks<T>{std::span{chunks}};
}

template<typename T>
[[nodiscard]] std::size_t ring_write_index(
    std::size_t capacity, std::size_t seek, std::size_t size) noexcept
{
    return (seek + size) % capacity;
}

template<typename T>
[[nodiscard]] std::span<T> ring_unused_capacity(
    T * data,
    std::size_t capacity,
    std::size_t seek,
    std::size_t size) noexcept
{
    if (size == capacity)
        return {};

    auto write = ring_write_index<T>(capacity, seek, size);
    auto n = write < seek ? seek - write : capacity - write;
    return std::span{data + write, std::min(n, capacity - size)};
}

} // namespace detail

/// Borrowed ring storage with explicit constructed-value lifetime.
///
/// `ring_region` is the pure storage/cursor layer below feeds and sinks. It
/// knows about borrowed storage, two-span views, raw writable tail
/// capacity, constructed prefixes, and destruction. It does not allocate,
/// suspend, or know about higher-level runtime ownership.
template<typename T>
class ring_region
{
public:
    using value_type = std::remove_cv_t<T>;
    using value_chunk_view = buffer_chunks<value_type>;
    using const_value_chunk_view = buffer_chunks<const value_type>;

    ring_region() = default;

    ring_region(value_type * data, std::size_t capacity)
        : data_(data)
        , capacity_(capacity)
    {
    }

    [[nodiscard]] value_type * data() noexcept
    {
        return data_;
    }

    [[nodiscard]] const value_type * data() const noexcept
    {
        return data_;
    }

    [[nodiscard]] std::span<value_type> storage() noexcept
    {
        return {data_, capacity_};
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] std::size_t unused_capacity_size() const noexcept
    {
        return capacity_ - size_;
    }

    [[nodiscard]] const_value_chunk_view constructed() const noexcept
    {
        return detail::ring_chunks(
            static_cast<const value_type *>(data_),
            capacity_,
            seek_,
            size_);
    }

    [[nodiscard]] value_chunk_view constructed() noexcept
    {
        return detail::ring_chunks(data_, capacity_, seek_, size_);
    }

    [[nodiscard]] std::span<value_type> unused_capacity() noexcept
    {
        return detail::ring_unused_capacity(data_, capacity_, seek_, size_);
    }

    [[nodiscard]] junk<value_type> unconstructed_capacity() noexcept
    {
        auto span = unused_capacity();
        return {span.data(), span.size()};
    }

    [[nodiscard]] value_type * front_data() noexcept
    {
        return empty() ? nullptr : data_ + seek_;
    }

    [[nodiscard]] const value_type * front_data() const noexcept
    {
        return empty() ? nullptr : data_ + seek_;
    }

    [[nodiscard]] std::size_t write_index() const noexcept
    {
        return detail::ring_write_index<value_type>(
            capacity_, seek_, size_);
    }

    [[nodiscard]] bool has_contiguous_constructed() const noexcept
    {
        return seek_ + size_ <= capacity_;
    }

    void advance_constructed(std::size_t n) noexcept
    {
        size_ += n;
    }

    void release_without_destroying() noexcept
    {
        seek_ = 0;
        size_ = 0;
    }

    void reset_if_empty() noexcept
    {
        if (empty())
            seek_ = 0;
    }

    void destroy_prefix(std::size_t n) noexcept
    {
        while (n != 0) {
            auto take = std::min(n, capacity_ - seek_);
            for (auto & value : std::span{data_ + seek_, take})
                std::destroy_at(&value);
            seek_ = (seek_ + take) % capacity_;
            size_ -= take;
            n -= take;
        }
    }

    void destroy_all() noexcept
    {
        destroy_prefix(size_);
        seek_ = 0;
    }

private:
    value_type * data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t seek_ = 0;
    std::size_t size_ = 0;
};

} // namespace nxtrt
