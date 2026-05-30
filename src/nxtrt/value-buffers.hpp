#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/task.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxtrt {

namespace detail {

template<typename Container, typename T>
concept push_back_value_container =
    requires(Container & container, T value) {
        container.push_back(std::move(value));
    };

template<typename Container, typename T>
concept insert_value_container =
    requires(Container & container, T value) {
        container.insert(container.end(), std::move(value));
    };

template<typename Container, typename T>
concept append_value_container =
    push_back_value_container<Container, T>
    || insert_value_container<Container, T>;

template<typename Container, typename T>
void append_value(Container & container, T value)
{
    if constexpr (push_back_value_container<Container, T>) {
        container.push_back(std::move(value));
    } else {
        container.insert(container.end(), std::move(value));
    }
}

} // namespace detail

struct value_buffer_error : runtime_error
{
    using runtime_error::runtime_error;
};

struct value_end_of_stream : value_buffer_error
{
    using value_buffer_error::value_buffer_error;
};

struct unexpected_value : value_buffer_error
{
    using value_buffer_error::value_buffer_error;
};

struct value_result
{
    /// Values accepted by the requested destination.
    ///
    /// Zero values is valid progresslessness; it only means EOF when `eof` is
    /// also true.
    std::size_t values = 0;
    /// True when the source is known to be exhausted.
    bool eof = false;
};

template<typename T>
using value_slot = std::optional<T>;

/// Buffered asynchronous value sink.
///
/// This is the typed-value counterpart to `byte_writer`: accepted values are
/// staged in object-lifetime-aware slots, and the virtual `drain_more()` is the
/// cold path for moving staged values into the concrete backend.
template<typename T>
class value_sink
{
public:
    using value_type = std::remove_cv_t<T>;
    using slot_type = value_slot<value_type>;

    explicit value_sink(std::span<slot_type> buffer)
        : buffer_(buffer)
    {}

    explicit value_sink(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_)
    {}

    value_sink(const value_sink &) = delete;
    value_sink & operator=(const value_sink &) = delete;

    value_sink(value_sink &&) = delete;
    value_sink & operator=(value_sink &&) = delete;

    virtual ~value_sink() = default;

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_ - seek_;
    }

    [[nodiscard]] std::size_t unused_capacity_size() const noexcept
    {
        return buffer_.size() - end_;
    }

    /// Accept one value into this sink.
    ///
    /// On a buffer hit this is ready immediately. On a miss the value is moved
    /// into a slow coroutine frame and remains alive there until the drain
    /// finishes.
    hope<void> write(value_type value)
    {
        if (unused_capacity_size() != 0) {
            emplace_back(std::move(value));
            return hope<void>::ready();
        }

        return write_slow(std::move(value));
    }

    /// Drain all currently buffered values to the concrete sink.
    hope<void> flush()
    {
        if (buffered_size() == 0) {
            reset_if_empty();
            return hope<void>::ready();
        }
        return flush_slow();
    }

protected:
    [[nodiscard]] std::span<slot_type> buffered_slots() noexcept
    {
        return buffer_.subspan(seek_, buffered_size());
    }

    /// Cold-path sink operation.
    ///
    /// Implementations accept a prefix of `values` and return how many values
    /// they accepted. Returning zero when any values are available is a protocol
    /// error for the base sink.
    virtual hope<std::size_t> drain_more(std::span<slot_type> values) = 0;

private:
    void emplace_back(value_type value)
    {
        if (unused_capacity_size() == 0)
            throw value_buffer_error{"value sink buffer is full"};
        buffer_[end_].emplace(std::move(value));
        ++end_;
    }

    void reset_if_empty() noexcept
    {
        if (seek_ == end_)
            seek_ = end_ = 0;
    }

    static void require_progress(
        std::size_t accepted,
        std::size_t available)
    {
        if (accepted == 0)
            throw value_buffer_error{"value sink made no progress"};
        if (accepted > available)
            throw value_buffer_error{"value sink overreported accepted values"};
    }

    void consume_buffered(std::size_t n)
    {
        if (n > buffered_size())
            throw value_buffer_error{"value sink consumed past buffer"};
        for (auto & slot : buffer_.subspan(seek_, n))
            slot.reset();
        seek_ += n;
        reset_if_empty();
    }

    task<void> flush_slow()
    {
        while (buffered_size() != 0) {
            auto values = buffered_slots();
            auto accepted = co_await drain_more(values);
            require_progress(accepted, values.size());
            consume_buffered(accepted);
        }
    }

    task<void> write_slow(value_type value)
    {
        if (buffer_.empty()) {
            auto slot = slot_type{std::move(value)};
            auto accepted = co_await drain_more(std::span{&slot, 1});
            require_progress(accepted, 1);
            slot.reset();
            co_return;
        }

        while (unused_capacity_size() == 0)
            co_await flush();

        emplace_back(std::move(value));
    }

    std::vector<slot_type> owned_buffer_;
    std::span<slot_type> buffer_;
    std::size_t seek_ = 0;
    std::size_t end_ = 0;
};

/// Sink that stores values in a fixed caller-owned slot buffer.
template<typename T>
class fixed_value_sink final : public value_sink<T>
{
public:
    using typename value_sink<T>::slot_type;

    explicit fixed_value_sink(std::span<slot_type> buffer)
        : value_sink<T>(buffer)
    {}

private:
    hope<std::size_t> drain_more(std::span<slot_type>) override
    {
        throw value_buffer_error{"fixed value sink is full"};
    }
};

/// Sink that accepts and ignores all values.
template<typename T>
class discarding_value_sink final : public value_sink<T>
{
public:
    using typename value_sink<T>::slot_type;

    explicit discarding_value_sink(std::span<slot_type> buffer = {})
        : value_sink<T>(buffer)
    {}

private:
    hope<std::size_t> drain_more(std::span<slot_type> values) override
    {
        return hope<std::size_t>::ready(values.size());
    }
};

/// Sink that appends accepted values directly into a container.
template<typename Container>
    requires detail::append_value_container<
        Container,
        typename Container::value_type>
class container_value_sink final
    : public value_sink<typename Container::value_type>
{
public:
    using value_type = typename Container::value_type;
    using slot_type = value_slot<value_type>;

    explicit container_value_sink(Container & container)
        : value_sink<value_type>(std::span<slot_type>{})
        , container_(&container)
    {}

private:
    hope<std::size_t> drain_more(std::span<slot_type> values) override
    {
        for (auto & value : values) {
            if (!value)
                throw value_buffer_error{"empty value slot"};
            detail::append_value(*container_, std::move(*value));
        }
        return hope<std::size_t>::ready(values.size());
    }

    Container * container_;
};

template<typename Container>
container_value_sink(Container &) -> container_value_sink<Container>;

/// Sink that writes accepted values directly through an output iterator.
template<typename T, std::output_iterator<std::remove_cv_t<T>> Output>
class iterator_value_sink final : public value_sink<T>
{
public:
    using value_type = std::remove_cv_t<T>;
    using slot_type = value_slot<value_type>;

    explicit iterator_value_sink(Output output)
        : value_sink<T>(std::span<slot_type>{})
        , output_(std::move(output))
    {}

    [[nodiscard]] Output output() const
    {
        return output_;
    }

private:
    hope<std::size_t> drain_more(std::span<slot_type> values) override
    {
        for (auto & value : values) {
            if (!value)
                throw value_buffer_error{"empty value slot"};
            *output_ = std::move(*value);
            ++output_;
        }
        return hope<std::size_t>::ready(values.size());
    }

    Output output_;
};

/// Buffered asynchronous value source.
///
/// This is the typed-value counterpart to `byte_reader`: lookahead lives in the
/// source's reusable value slots, while `stream_more()` is the cold path that
/// produces more values from the concrete source.
template<typename T>
class value_source
{
public:
    using value_type = std::remove_cv_t<T>;
    using slot_type = value_slot<value_type>;

    explicit value_source(std::span<slot_type> buffer)
        : buffer_(buffer)
    {
        if (buffer.empty())
            throw value_buffer_error{"value source buffer is empty"};
    }

    explicit value_source(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_)
    {
        if (owned_buffer_.empty())
            throw value_buffer_error{"value source buffer is empty"};
    }

    value_source(const value_source &) = delete;
    value_source & operator=(const value_source &) = delete;

    value_source(value_source &&) = delete;
    value_source & operator=(value_source &&) = delete;

    virtual ~value_source() = default;

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_ - seek_;
    }

    [[nodiscard]] std::size_t unused_capacity_size() const noexcept
    {
        return buffer_.size() - end_;
    }

    /// Ensure at least `n` values are buffered.
    ///
    /// If EOF occurs before `n` values are available, throws
    /// `value_end_of_stream`.
    hope<void> fill(std::size_t n)
    {
        if (n > buffer_.size())
            throw value_buffer_error{"value source buffer is too small"};
        if (buffered_size() >= n)
            return hope<void>::ready();
        return fill_slow(n);
    }

    /// Borrow the next value without consuming it.
    ///
    /// Returns `nullptr` at EOF. The pointer is invalidated by any operation
    /// that refills, rebases, or consumes this source.
    hope<const value_type *> peek()
    {
        if (buffered_size() != 0)
            return hope<const value_type *>::ready(&*buffer_[seek_]);

        auto read = fill_more();
        if (!read.is_ready())
            return peek_slow(std::move(read));

        auto result = read.take_ready();
        if (buffered_size() != 0)
            return hope<const value_type *>::ready(&*buffer_[seek_]);
        if (result.eof && result.values == 0)
            return hope<const value_type *>::ready(nullptr);
        return peek_slow();
    }

    /// Consume and return the next value.
    ///
    /// Returns `std::nullopt` at EOF.
    hope<std::optional<value_type>> take()
    {
        if (buffered_size() != 0)
            return hope<std::optional<value_type>>::ready(take_buffered());

        auto read = fill_more();
        if (!read.is_ready())
            return take_slow(std::move(read));

        auto result = read.take_ready();
        if (buffered_size() != 0)
            return hope<std::optional<value_type>>::ready(take_buffered());
        if (result.eof && result.values == 0)
            return hope<std::optional<value_type>>::ready(std::nullopt);
        return take_slow();
    }

    /// Borrow the next value, or throw `value_end_of_stream` at EOF.
    hope<const value_type *> peek_one()
    {
        auto value = peek();
        if (value.is_ready()) {
            auto * one = value.take_ready();
            if (one == nullptr)
                throw value_end_of_stream{"unexpected end of value input"};
            return hope<const value_type *>::ready(one);
        }
        return peek_one_slow(std::move(value));
    }

    /// Consume and return the next value, or throw `value_end_of_stream` at EOF.
    hope<value_type> take_one()
    {
        auto value = take();
        if (value.is_ready()) {
            auto one = value.take_ready();
            if (!one)
                throw value_end_of_stream{"unexpected end of value input"};
            return hope<value_type>::ready(std::move(*one));
        }
        return take_one_slow(std::move(value));
    }

    /// Consume one expected value, or throw without consuming on mismatch.
    hope<void> expect(value_type expected)
        requires std::equality_comparable<value_type>
    {
        if (buffered_size() != 0) {
            expect_buffered(expected);
            return hope<void>::ready();
        }
        return expect_slow(std::move(expected));
    }

    /// Consume all expected values in order.
    template<typename... Expected>
        requires std::equality_comparable<value_type>
            && (std::constructible_from<value_type, Expected &&> && ...)
    task<void> discard_all(Expected &&... expected)
    {
        if constexpr (sizeof...(Expected) != 0) {
            auto values = std::array<value_type, sizeof...(Expected)>{
                value_type{std::forward<Expected>(expected)}...,
            };
            for (auto & value : values)
                co_await expect(std::move(value));
        }
    }

    /// Transfer one available chunk to `sink`, up to `limit` values.
    hope<value_result> stream(
        value_sink<value_type> & sink,
        std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (limit == 0)
            return hope<value_result>::ready(value_result{});

        if (buffered_size() == 0)
            return stream_more(sink, limit);

        return stream_buffered(sink, limit);
    }

    /// Discard one available chunk, up to `limit` values.
    hope<value_result> discard(
        std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (limit == 0)
            return hope<value_result>::ready(value_result{});

        if (buffered_size() == 0)
            return discard_more(limit);

        auto n = std::min(limit, buffered_size());
        toss(n);
        return hope<value_result>::ready(value_result{.values = n});
    }

protected:
    [[nodiscard]] std::span<slot_type> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    void emplace(value_type value)
    {
        if (unused_capacity_size() == 0)
            throw value_buffer_error{"value source buffer is full"};
        buffer_[end_].emplace(std::move(value));
        ++end_;
    }

    /// Mandatory cold-path source operation.
    ///
    /// Implementations should transfer up to `limit` values from the underlying
    /// source into `sink`, returning how many values were logically advanced.
    /// A zero value count does not by itself mean EOF.
    ///
    /// Like `byte_reader::stream_more()`, an implementation may instead append
    /// values to this source's own buffer with `emplace()` and return
    /// `{.values = 0, .eof = false}`. That is the fallback when the destination
    /// cannot immediately accept a value and the source has local buffer space.
    virtual hope<value_result> stream_more(
        value_sink<value_type> & sink,
        std::size_t limit) = 0;

    virtual hope<value_result> discard_more(std::size_t limit)
    {
        auto sink = discarding_value_sink<value_type>{};
        auto result = stream_more(sink, limit);
        if (result.is_ready())
            return hope<value_result>::ready(result.take_ready());
        return discard_more_slow(limit);
    }

    virtual void rebase_more(std::size_t)
    {
        auto pending = buffered_size();
        if (seek_ != 0 && pending != 0) {
            for (auto i = std::size_t{0}; i < pending; ++i) {
                auto from = seek_ + i;
                buffer_[i].emplace(std::move(*buffer_[from]));
                buffer_[from].reset();
            }
        }
        seek_ = 0;
        end_ = pending;
    }

private:
    void rebase(std::size_t capacity)
    {
        if (capacity > buffer_.size())
            throw value_buffer_error{"value source buffer is too small"};
        if (buffer_.size() - seek_ >= capacity)
            return;
        rebase_more(capacity);
        if (buffer_.size() - seek_ < capacity)
            throw value_buffer_error{"value source rebase failed"};
    }

    void toss(std::size_t n)
    {
        if (n > buffered_size())
            throw value_buffer_error{"value source consumed past buffer"};
        for (auto & slot : buffer_.subspan(seek_, n))
            slot.reset();
        seek_ += n;
        reset_if_empty();
    }

    std::optional<value_type> take_buffered()
    {
        auto value = std::optional<value_type>{
            std::move(*buffer_[seek_]),
        };
        toss(1);
        return value;
    }

    void expect_buffered(const value_type & expected)
        requires std::equality_comparable<value_type>
    {
        if (!(*buffer_[seek_] == expected))
            throw unexpected_value{"unexpected value"};
        toss(1);
    }

    void reset_if_empty() noexcept
    {
        if (seek_ == end_)
            seek_ = end_ = 0;
    }

    hope<value_result> fill_more()
    {
        if (buffered_size() == buffer_.size())
            throw value_buffer_error{"value source buffer is full"};
        rebase(buffered_size() + 1);
        return fill_more_without_rebase();
    }

    hope<value_result> fill_more_without_rebase()
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw value_buffer_error{"value source buffer is full"};

        auto sink = fixed_value_sink<value_type>{dst};
        auto result = stream_more(sink, dst.size());
        if (result.is_ready())
            return hope<value_result>::ready(
                finish_read(sink, result.take_ready()));
        return read_more_slow(dst.size());
    }

    value_result finish_read(
        fixed_value_sink<value_type> & sink,
        value_result result)
    {
        auto written = sink.buffered_size();
        if (written != result.values)
            throw value_buffer_error{"value source stream count mismatch"};
        end_ += written;
        return result;
    }

    task<value_result> read_more_slow(std::size_t limit)
    {
        auto sink = fixed_value_sink<value_type>{
            unused_capacity().first(limit),
        };
        auto result = co_await stream_more(sink, limit);
        co_return finish_read(sink, result);
    }

    task<void> fill_slow(std::size_t n)
    {
        rebase(n);
        while (buffered_size() < n) {
            auto before = buffered_size();
            auto read = co_await fill_more_without_rebase();
            if (read.eof && read.values == 0 && buffered_size() == before)
                throw value_end_of_stream{"unexpected end of value input"};
        }
    }

    task<const value_type *> peek_slow()
    {
        while (true) {
            auto before = buffered_size();
            auto read = co_await fill_more();
            if (buffered_size() != 0)
                co_return &*buffer_[seek_];
            if (read.eof && read.values == 0 && buffered_size() == before)
                co_return nullptr;
        }
    }

    task<const value_type *> peek_slow(hope<value_result> first_read)
    {
        auto before = buffered_size();
        auto read = co_await std::move(first_read);
        if (buffered_size() != 0)
            co_return &*buffer_[seek_];
        if (read.eof && read.values == 0 && buffered_size() == before)
            co_return nullptr;
        co_return co_await peek_slow();
    }

    task<std::optional<value_type>> take_slow()
    {
        while (true) {
            auto before = buffered_size();
            auto read = co_await fill_more();
            if (buffered_size() != 0)
                co_return take_buffered();
            if (read.eof && read.values == 0 && buffered_size() == before)
                co_return std::nullopt;
        }
    }

    task<std::optional<value_type>> take_slow(hope<value_result> first_read)
    {
        auto before = buffered_size();
        auto read = co_await std::move(first_read);
        if (buffered_size() != 0)
            co_return take_buffered();
        if (read.eof && read.values == 0 && buffered_size() == before)
            co_return std::nullopt;
        co_return co_await take_slow();
    }

    task<const value_type *> peek_one_slow(
        hope<const value_type *> value)
    {
        auto * one = co_await std::move(value);
        if (one == nullptr)
            throw value_end_of_stream{"unexpected end of value input"};
        co_return one;
    }

    task<value_type> take_one_slow(
        hope<std::optional<value_type>> value)
    {
        auto one = co_await std::move(value);
        if (!one)
            throw value_end_of_stream{"unexpected end of value input"};
        co_return std::move(*one);
    }

    task<void> expect_slow(value_type expected)
        requires std::equality_comparable<value_type>
    {
        auto * value = co_await peek();
        if (value == nullptr)
            throw value_end_of_stream{"unexpected end of value input"};
        if (!(*value == expected))
            throw unexpected_value{"unexpected value"};
        co_await discard(1);
    }

    hope<value_result> stream_buffered(
        value_sink<value_type> & sink,
        std::size_t limit)
    {
        auto n = std::min(limit, buffered_size());
        auto moved = std::size_t{0};
        while (moved != n) {
            auto value = std::move(*buffer_[seek_]);
            toss(1);
            auto write = sink.write(std::move(value));
            ++moved;
            if (!write.is_ready())
                return stream_buffered_slow(std::move(write), moved);
        }

        return hope<value_result>::ready(value_result{.values = moved});
    }

    task<value_result> stream_buffered_slow(
        hope<void> write,
        std::size_t moved)
    {
        co_await std::move(write);
        co_return value_result{.values = moved};
    }

    task<value_result> discard_more_slow(std::size_t limit)
    {
        auto sink = discarding_value_sink<value_type>{};
        co_return co_await stream_more(sink, limit);
    }

    std::vector<slot_type> owned_buffer_;
    std::span<slot_type> buffer_;
    std::size_t seek_ = 0;
    std::size_t end_ = 0;
};

/// Source that repeatedly parses typed values from a byte reader.
template<typename T>
class byte_parser final : public value_source<T>
{
public:
    using value_type = std::remove_cv_t<T>;
    using slot_type = value_slot<value_type>;
    using parser_type = task<std::optional<value_type>> (*)(byte_reader &);

    byte_parser(
        byte_reader & reader,
        parser_type parser,
        std::size_t buffer_size = 1)
        : value_source<value_type>(buffer_size)
        , reader_(&reader)
        , parser_(parser)
    {
        if (parser_ == nullptr)
            throw value_buffer_error{"byte parser function is null"};
    }

    byte_parser(
        byte_reader & reader,
        parser_type parser,
        std::span<slot_type> buffer)
        : value_source<value_type>(buffer)
        , reader_(&reader)
        , parser_(parser)
    {
        if (parser_ == nullptr)
            throw value_buffer_error{"byte parser function is null"};
    }

private:
    hope<value_result> stream_more(
        value_sink<value_type> & sink,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<value_result>::ready(value_result{});
        return stream_more_task(sink);
    }

    task<value_result> stream_more_task(value_sink<value_type> & sink)
    {
        auto value = co_await parser_(*reader_);
        if (!value)
            co_return value_result{.eof = true};

        co_await sink.write(std::move(*value));
        co_return value_result{.values = 1};
    }

    byte_reader * reader_;
    parser_type parser_;
};

/// Source backed by a single-pass range or lazy view of values.
template<std::ranges::input_range Values>
    requires std::ranges::view<Values>
        && std::constructible_from<
            std::remove_cv_t<std::ranges::range_value_t<Values>>,
            std::ranges::range_rvalue_reference_t<Values>>
class value_range_source final
    : public value_source<std::ranges::range_value_t<Values>>
{
public:
    using value_type =
        std::remove_cv_t<std::ranges::range_value_t<Values>>;
    using slot_type = value_slot<value_type>;

    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Values, std::views::all_t<Range>>
    value_range_source(Range && values, std::span<slot_type> buffer)
        : value_source<value_type>(buffer)
        , values_(std::views::all(std::forward<Range>(values)))
        , value_(std::ranges::begin(values_))
        , end_(std::ranges::end(values_))
    {}

    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Values, std::views::all_t<Range>>
    explicit value_range_source(
        Range && values,
        std::size_t buffer_size = 1)
        : value_source<value_type>(buffer_size)
        , values_(std::views::all(std::forward<Range>(values)))
        , value_(std::ranges::begin(values_))
        , end_(std::ranges::end(values_))
    {}

private:
    hope<value_result> stream_more(
        value_sink<value_type> & sink,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<value_result>::ready(value_result{});

        auto total = std::size_t{0};
        while (value_ != end_ && total != limit) {
            auto value = value_type{std::ranges::iter_move(value_)};
            auto write = sink.write(std::move(value));
            if (!write.is_ready())
                return stream_write_slow(std::move(write), total);

            ++value_;
            ++total;
            if (sink.unused_capacity_size() == 0)
                break;
        }

        return hope<value_result>::ready(
            value_result{.values = total, .eof = value_ == end_});
    }

    task<value_result> stream_write_slow(
        hope<void> write,
        std::size_t prefix)
    {
        co_await std::move(write);
        ++value_;
        co_return value_result{
            .values = prefix + 1,
            .eof = value_ == end_,
        };
    }

    Values values_;
    std::ranges::iterator_t<Values> value_;
    std::ranges::sentinel_t<Values> end_;
};

template<std::ranges::viewable_range Range>
value_range_source(
    Range &&,
    std::span<value_slot<std::ranges::range_value_t<std::views::all_t<Range>>>>)
    -> value_range_source<std::views::all_t<Range>>;

template<std::ranges::viewable_range Range>
value_range_source(Range &&)
    -> value_range_source<std::views::all_t<Range>>;

template<std::ranges::viewable_range Range>
value_range_source(Range &&, std::size_t)
    -> value_range_source<std::views::all_t<Range>>;

/// Stream all values from `source` into `sink`, then flush `sink`.
template<typename T>
task<std::size_t> stream_all(
    value_source<T> & source,
    value_sink<std::remove_cv_t<T>> & sink)
{
    auto total = std::size_t{0};
    while (true) {
        auto result = co_await source.stream(sink);
        total += result.values;
        if (result.eof)
            break;
    }
    co_await sink.flush();
    co_return total;
}

} // namespace nxtrt
