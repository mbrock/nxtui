#pragma once

#include "nxtrt/buffer-core.hpp"
#include "nxtrt/task.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxtrt {

class byte_reader;

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

template<typename T, std::size_t Inline = 2>
using value_chunks = buffer_chunks<T, Inline>;

template<typename T>
struct value_storage_ref
{
    using value_type = std::remove_cv_t<T>;

    value_storage_ref() = default;

    value_storage_ref(value_type * data, std::size_t size)
        : data(data)
        , size(size)
    {}

    value_storage_ref(std::span<value_type> storage)
        : data(storage.data())
        , size(storage.size())
    {}

    value_type * data = nullptr;
    std::size_t size = 0;
};

template<typename T>
class value_storage
{
public:
    using value_type = std::remove_cv_t<T>;

    explicit value_storage(std::size_t size)
        : data_(size == 0 ? nullptr : allocator_.allocate(size))
        , size_(size)
    {}

    value_storage(const value_storage &) = delete;
    value_storage & operator=(const value_storage &) = delete;

    value_storage(value_storage && other) noexcept
        : data_(std::exchange(other.data_, nullptr))
        , size_(std::exchange(other.size_, 0))
    {}

    value_storage & operator=(value_storage && other) noexcept
    {
        if (this != &other) {
            deallocate();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~value_storage()
    {
        deallocate();
    }

    [[nodiscard]] value_type * data() noexcept
    {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] value_storage_ref<value_type> ref() & noexcept
    {
        return {data_, size_};
    }

    [[nodiscard]] operator value_storage_ref<value_type>() & noexcept
    {
        return ref();
    }

    operator value_storage_ref<value_type>() && = delete;

private:
    void deallocate() noexcept
    {
        if (data_ != nullptr)
            allocator_.deallocate(data_, size_);
        data_ = nullptr;
        size_ = 0;
    }

    [[no_unique_address]] std::allocator<value_type> allocator_;
    value_type * data_ = nullptr;
    std::size_t size_ = 0;
};

template<typename T, std::size_t N>
class static_value_storage
{
public:
    using value_type = std::remove_cv_t<T>;

    static_value_storage() = default;

    static_value_storage(const static_value_storage &) = delete;
    static_value_storage & operator=(const static_value_storage &) = delete;

    [[nodiscard]] value_type * data() noexcept
    {
        return std::launder(reinterpret_cast<value_type *>(storage_));
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return N;
    }

    [[nodiscard]] value_storage_ref<value_type> ref() & noexcept
    {
        return {data(), N};
    }

    [[nodiscard]] operator value_storage_ref<value_type>() & noexcept
    {
        return ref();
    }

    operator value_storage_ref<value_type>() && = delete;

private:
    alignas(value_type) std::byte storage_[
        sizeof(value_type) * (N == 0 ? 1 : N)];
};

/// Buffered asynchronous value sink.
///
/// This is the typed-value counterpart to `byte_writer`: accepted values are
/// constructed in raw value storage, and the virtual `drain_more()` is the cold
/// path for moving staged values into the concrete backend.
template<typename T>
class value_sink
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = value_storage_ref<value_type>;
    using value_chunk_view = value_chunks<value_type>;
    using const_value_chunk_view = value_chunks<const value_type>;

    explicit value_sink(storage_ref buffer)
        : buffer_(buffer.data)
        , capacity_(buffer.size)
    {}

    explicit value_sink(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_.data())
        , capacity_(owned_buffer_.size())
    {}

    value_sink(const value_sink &) = delete;
    value_sink & operator=(const value_sink &) = delete;

    value_sink(value_sink &&) = delete;
    value_sink & operator=(value_sink &&) = delete;

    virtual ~value_sink()
    {
        destroy_buffered();
    }

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] std::size_t unused_capacity_size() const noexcept
    {
        return capacity_ - size_;
    }

    /// Return values currently staged for the sink.
    ///
    /// The returned chunks are invalidated by any operation that drains,
    /// flushes, or appends to this sink.
    [[nodiscard]] const_value_chunk_view buffered() const noexcept
    {
        return buffered_chunks();
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

    /// Accept a contiguous run of values into this sink.
    hope<void> write(std::span<const value_type> values)
        requires std::copy_constructible<value_type>
    {
        if (values.empty())
            return hope<void>::ready();
        if (values.size() <= unused_capacity().size()) {
            append_to_buffer(values);
            return hope<void>::ready();
        }
        return write_slow(values);
    }

    /// Accept a chunked run of values into this sink.
    template<std::size_t Inline>
    hope<void> write(value_chunks<const value_type, Inline> values)
        requires std::copy_constructible<value_type>
    {
        if (values.empty())
            return hope<void>::ready();
        if (values.size() <= unused_capacity().size()) {
            append_to_buffer(values);
            return hope<void>::ready();
        }
        return write_slow(values);
    }

    /// Accept `pattern` repeated `splat` times.
    hope<void> write_splat(
        std::span<const value_type> pattern,
        std::size_t splat)
        requires std::copy_constructible<value_type>
    {
        if (pattern.empty() || splat == 0)
            return hope<void>::ready();
        auto count = repeated_size(pattern.size(), splat);
        if (count <= unused_capacity().size()) {
            append_splat_to_buffer(pattern, splat);
            return hope<void>::ready();
        }
        return write_splat_slow(pattern, splat);
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
    [[nodiscard]] std::span<value_type> unused_capacity() noexcept
    {
        return detail::ring_unused_capacity(
            buffer_,
            capacity_,
            seek_,
            size_);
    }

    void advance_constructed(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw value_buffer_error{"value sink advanced past buffer capacity"};
        size_ += n;
    }

    [[nodiscard]] const_value_chunk_view buffered_chunks() const noexcept
    {
        return detail::ring_chunks(
            static_cast<const value_type *>(buffer_),
            capacity_,
            seek_,
            size_);
    }

    [[nodiscard]] value_chunk_view buffered_values() noexcept
    {
        return detail::ring_chunks(buffer_, capacity_, seek_, size_);
    }

    void release_buffered_without_destroying() noexcept
    {
        seek_ = size_ = 0;
    }

    void consume_buffered_for_derived(std::size_t n)
    {
        consume_buffered(n);
    }

    /// Cold-path sink operation.
    ///
    /// Implementations accept a prefix of `values` and return how many values
    /// they accepted. Returning zero when any values are available is a protocol
    /// error for the base sink.
    virtual hope<std::size_t> drain_more(value_chunk_view values) = 0;

private:
    void emplace_back(value_type value)
    {
        if (unused_capacity_size() == 0)
            throw value_buffer_error{"value sink buffer is full"};
        std::construct_at(buffer_ + write_index(), std::move(value));
        ++size_;
    }

    static std::size_t repeated_size(
        std::size_t pattern_size,
        std::size_t splat)
    {
        if (
            pattern_size != 0
            && splat > std::numeric_limits<std::size_t>::max() / pattern_size)
            throw value_buffer_error{"value count overflow"};
        return pattern_size * splat;
    }

    void append_to_buffer(std::span<const value_type> values)
        requires std::copy_constructible<value_type>
    {
        auto dst = unused_capacity();
        if (values.size() > dst.size())
            throw value_buffer_error{"value sink buffer is full"};
        for (auto i = std::size_t{0}; i < values.size(); ++i)
            std::construct_at(dst.data() + i, values[i]);
        size_ += values.size();
    }

    template<std::size_t Inline>
    void append_to_buffer(value_chunks<const value_type, Inline> values)
        requires std::copy_constructible<value_type>
    {
        for (auto chunk : values)
            append_to_buffer(chunk);
    }

    void append_splat_to_buffer(
        std::span<const value_type> pattern,
        std::size_t splat)
        requires std::copy_constructible<value_type>
    {
        for (auto i = std::size_t{0}; i < splat; ++i)
            append_to_buffer(pattern);
    }

    void reset_if_empty() noexcept
    {
        if (size_ == 0)
            seek_ = 0;
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
        destroy_prefix(n);
        reset_if_empty();
    }

    void destroy_buffered() noexcept
    {
        destroy_prefix(size_);
        seek_ = 0;
    }

    task<void> flush_slow()
    {
        while (buffered_size() != 0)
            co_await drain_buffered_once();
    }

    task<void> write_slow(value_type value)
    {
        if (capacity_ == 0) {
            auto one = value_type{std::move(value)};
            auto accepted = co_await drain_more(value_chunk_view{
                std::span{&one, 1},
            });
            require_progress(accepted, 1);
            co_return;
        }

        while (unused_capacity_size() == 0)
            co_await drain_buffered_once();

        emplace_back(std::move(value));
    }

    task<void> write_slow(std::span<const value_type> values)
        requires std::copy_constructible<value_type>
    {
        for (auto value : values)
            co_await write(value_type{value});
    }

    template<std::size_t Inline>
    task<void> write_slow(value_chunks<const value_type, Inline> values)
        requires std::copy_constructible<value_type>
    {
        for (auto chunk : values)
            co_await write(chunk);
    }

    task<void> write_splat_slow(
        std::span<const value_type> pattern,
        std::size_t splat)
        requires std::copy_constructible<value_type>
    {
        for (auto i = std::size_t{0}; i < splat; ++i)
            co_await write(pattern);
    }

    task<void> drain_buffered_once()
    {
        auto values = buffered_values();
        auto accepted = co_await drain_more(values);
        require_progress(accepted, values.size());
        consume_buffered(accepted);
    }

    value_storage<value_type> owned_buffer_{0};
    value_type * buffer_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t seek_ = 0;
    std::size_t size_ = 0;

    [[nodiscard]] std::size_t write_index() const noexcept
    {
        return detail::ring_write_index<value_type>(capacity_, seek_, size_);
    }

    void destroy_prefix(std::size_t n) noexcept
    {
        while (n != 0) {
            auto take = std::min(n, capacity_ - seek_);
            for (auto & value : std::span{buffer_ + seek_, take})
                std::destroy_at(&value);
            seek_ = (seek_ + take) % capacity_;
            size_ -= take;
            n -= take;
        }
    }
};

/// Sink that stores values in fixed caller-owned raw storage.
template<typename T>
class fixed_value_sink final : public value_sink<T>
{
public:
    using typename value_sink<T>::storage_ref;

    explicit fixed_value_sink(storage_ref buffer)
        : value_sink<T>(buffer)
    {}

    void release_buffered() noexcept
    {
        this->release_buffered_without_destroying();
    }

private:
    hope<std::size_t>
    drain_more(typename value_sink<T>::value_chunk_view) override
    {
        throw value_buffer_error{"fixed value sink is full"};
    }
};

/// Sink that accepts and ignores all values.
template<typename T>
class discarding_value_sink final : public value_sink<T>
{
public:
    using typename value_sink<T>::storage_ref;

    explicit discarding_value_sink(storage_ref buffer = {})
        : value_sink<T>(buffer)
    {}

private:
    hope<std::size_t>
    drain_more(typename value_sink<T>::value_chunk_view values) override
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

    explicit container_value_sink(Container & container)
        : value_sink<value_type>(value_storage_ref<value_type>{})
        , container_(&container)
    {}

private:
    hope<std::size_t> drain_more(
        typename value_sink<value_type>::value_chunk_view values) override
    {
        auto total = std::size_t{0};
        for (auto chunk : values) {
            total += chunk.size();
            for (auto & value : chunk)
                detail::append_value(*container_, std::move(value));
        }
        return hope<std::size_t>::ready(total);
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

    explicit iterator_value_sink(Output output)
        : value_sink<T>(value_storage_ref<value_type>{})
        , output_(std::move(output))
    {}

    [[nodiscard]] Output output() const
    {
        return output_;
    }

private:
    hope<std::size_t> drain_more(
        typename value_sink<T>::value_chunk_view values) override
    {
        auto total = std::size_t{0};
        for (auto chunk : values) {
            total += chunk.size();
            for (auto & value : chunk) {
                *output_ = std::move(value);
                ++output_;
            }
        }
        return hope<std::size_t>::ready(total);
    }

    Output output_;
};

/// Buffered asynchronous value source.
///
/// This is the typed-value counterpart to `byte_reader`: lookahead lives in the
/// source's reusable value storage, while `stream_more()` is the cold path that
/// produces more values from the concrete source.
template<typename T>
class value_source
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = value_storage_ref<value_type>;
    using value_chunk_view = value_chunks<value_type>;
    using const_value_chunk_view = value_chunks<const value_type>;

    explicit value_source(storage_ref buffer)
        : buffer_(buffer.data)
        , capacity_(buffer.size)
    {
        if (capacity_ == 0)
            throw value_buffer_error{"value source buffer is empty"};
    }

    explicit value_source(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_.data())
        , capacity_(owned_buffer_.size())
    {
        if (capacity_ == 0)
            throw value_buffer_error{"value source buffer is empty"};
    }

    value_source(const value_source &) = delete;
    value_source & operator=(const value_source &) = delete;

    value_source(value_source &&) = delete;
    value_source & operator=(value_source &&) = delete;

    virtual ~value_source()
    {
        destroy_buffered();
    }

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] std::size_t unused_capacity_size() const noexcept
    {
        return capacity_ - size_;
    }

    /// Return currently buffered values.
    ///
    /// The returned chunks are invalidated by any operation that refills,
    /// streams, discards, or consumes this source.
    [[nodiscard]] const_value_chunk_view buffered() const noexcept
    {
        return buffered_chunks();
    }

    /// Ensure at least `n` values are buffered.
    ///
    /// If EOF occurs before `n` values are available, throws
    /// `value_end_of_stream`.
    hope<void> fill(std::size_t n)
    {
        if (n > capacity_)
            throw value_buffer_error{"value source buffer is too small"};
        if (buffered_size() >= n)
            return hope<void>::ready();
        return fill_slow(n);
    }

    /// Borrow the next value without consuming it.
    ///
    /// Returns `nullptr` at EOF. The pointer is invalidated by any operation
    /// that refills or consumes this source.
    hope<const value_type *> peek()
    {
        if (buffered_size() != 0)
            return hope<const value_type *>::ready(buffer_ + seek_);

        auto read = fill_more();
        if (!read.is_ready())
            return peek_slow(std::move(read));

        auto result = read.take_ready();
        if (buffered_size() != 0)
            return hope<const value_type *>::ready(buffer_ + seek_);
        if (result.eof && result.values == 0)
            return hope<const value_type *>::ready(nullptr);
        return peek_slow();
    }

    /// Borrow the next `n` values without consuming them.
    ///
    /// The returned chunks remain valid only until the next operation that may
    /// mutate this source's buffer.
    hope<const_value_chunk_view> peek(std::size_t n)
    {
        if (n > capacity_)
            throw value_buffer_error{"value source buffer is too small"};
        if (buffered_size() >= n)
            return hope<const_value_chunk_view>::ready(buffered().first(n));
        return peek_slow(n);
    }

    /// Copy a trivially copyable object from the next values without consuming.
    ///
    /// This is byte-reader `peek_struct()` generalized over atom-sized source
    /// values: for example, three buffered `int` values may be copied into a
    /// trivially copyable struct whose object representation is three ints wide.
    template<typename Object>
        requires std::is_trivially_copyable_v<Object>
            && std::is_trivially_copyable_v<value_type>
    hope<Object> peek_struct()
    {
        constexpr auto n = object_value_count<Object>();
        auto values = peek(n);
        if (values.is_ready())
            return hope<Object>::ready(copy_struct<Object>(values.take_ready()));
        return peek_struct_slow<Object>(std::move(values));
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

    /// Consume and copy a trivially copyable object from the next values.
    ///
    /// Returns `std::nullopt` only when EOF is reached before any value of the
    /// object is available. EOF in the middle of the object is
    /// `value_end_of_stream`.
    template<typename Object>
        requires std::is_trivially_copyable_v<Object>
            && std::is_trivially_copyable_v<value_type>
    hope<std::optional<Object>> take_struct()
    {
        constexpr auto n = object_value_count<Object>();
        if (buffered_size() >= n) {
            auto value = copy_struct<Object>(buffered().first(n));
            toss(n);
            return hope<std::optional<Object>>::ready(value);
        }

        return take_struct_slow<Object>();
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
    [[nodiscard]] const_value_chunk_view buffered_chunks() const noexcept
    {
        return detail::ring_chunks(
            static_cast<const value_type *>(buffer_),
            capacity_,
            seek_,
            size_);
    }

    [[nodiscard]] std::size_t storage_capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] std::span<value_type> buffer_storage() noexcept
    {
        return {buffer_, capacity_};
    }

    [[nodiscard]] std::span<value_type> unused_capacity() noexcept
    {
        return detail::ring_unused_capacity(
            buffer_,
            capacity_,
            seek_,
            size_);
    }

    void emplace(value_type value)
    {
        if (unused_capacity_size() == 0)
            throw value_buffer_error{"value source buffer is full"};
        std::construct_at(buffer_ + write_index(), std::move(value));
        ++size_;
    }

    void advance_constructed(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw value_buffer_error{"value source advanced past buffer capacity"};
        size_ += n;
    }

    void consume_buffered_for_derived(std::size_t n)
    {
        toss(n);
    }

    void contiguize_buffered_for_derived()
        requires std::is_trivially_copyable_v<value_type>
    {
        if (size_ == 0) {
            seek_ = 0;
            return;
        }
        if (seek_ + size_ <= capacity_)
            return;

        auto values = std::vector<value_type>(size_);
        auto offset = std::size_t{0};
        for (auto chunk : buffered_chunks()) {
            std::memcpy(
                values.data() + offset,
                chunk.data(),
                chunk.size_bytes());
            offset += chunk.size();
        }

        destroy_prefix(size_);
        seek_ = 0;
        for (auto i = std::size_t{0}; i < values.size(); ++i)
            std::construct_at(buffer_ + i, values[i]);
        size_ = values.size();
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

private:
    void toss(std::size_t n)
    {
        if (n > buffered_size())
            throw value_buffer_error{"value source consumed past buffer"};
        destroy_prefix(n);
        reset_if_empty();
    }

    std::optional<value_type> take_buffered()
    {
        auto value = std::optional<value_type>{
            std::move(buffer_[seek_]),
        };
        toss(1);
        return value;
    }

    void expect_buffered(const value_type & expected)
        requires std::equality_comparable<value_type>
    {
        if (!(buffer_[seek_] == expected))
            throw unexpected_value{"unexpected value"};
        toss(1);
    }

    void reset_if_empty() noexcept
    {
        if (size_ == 0)
            seek_ = 0;
    }

    hope<value_result> fill_more()
    {
        if (buffered_size() == capacity_)
            throw value_buffer_error{"value source buffer is full"};
        return fill_more_into_capacity();
    }

    hope<value_result> fill_more_into_capacity()
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
        sink.release_buffered();
        size_ += written;
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
        while (buffered_size() < n) {
            auto before = buffered_size();
            auto read = co_await fill_more_into_capacity();
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
                co_return buffer_ + seek_;
            if (read.eof && read.values == 0 && buffered_size() == before)
                co_return nullptr;
        }
    }

    task<const value_type *> peek_slow(hope<value_result> first_read)
    {
        auto before = buffered_size();
        auto read = co_await std::move(first_read);
        if (buffered_size() != 0)
            co_return buffer_ + seek_;
        if (read.eof && read.values == 0 && buffered_size() == before)
            co_return nullptr;
        co_return co_await peek_slow();
    }

    task<const_value_chunk_view> peek_slow(std::size_t n)
    {
        co_await fill(n);
        co_return buffered().first(n);
    }

    template<typename Object>
        requires std::is_trivially_copyable_v<Object>
            && std::is_trivially_copyable_v<value_type>
    task<Object> peek_struct_slow(hope<const_value_chunk_view> values)
    {
        co_return copy_struct<Object>(co_await std::move(values));
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

    template<typename Object>
        requires std::is_trivially_copyable_v<Object>
            && std::is_trivially_copyable_v<value_type>
    task<std::optional<Object>> take_struct_slow()
    {
        constexpr auto n = object_value_count<Object>();
        if (buffered_size() < n) {
            while (buffered_size() < n) {
                auto before = buffered_size();
                auto read = co_await fill_more();
                if (
                    read.eof
                    && read.values == 0
                    && buffered_size() == before) {
                    if (buffered_size() == 0)
                        co_return std::nullopt;
                    throw value_end_of_stream{
                        "unexpected end of value input",
                    };
                }
            }
        }

        auto value = copy_struct<Object>(buffered().first(n));
        toss(n);
        co_return value;
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
            auto value = std::move(buffer_[seek_]);
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

    void destroy_buffered() noexcept
    {
        destroy_prefix(size_);
        seek_ = 0;
    }

    template<typename Object>
    static consteval std::size_t object_value_count()
    {
        static_assert(
            sizeof(Object) % sizeof(value_type) == 0,
            "object size must be a whole number of source values");
        return sizeof(Object) / sizeof(value_type);
    }

    template<typename Object, std::size_t Inline>
    static Object copy_struct(value_chunks<const value_type, Inline> values)
    {
        constexpr auto n = object_value_count<Object>();
        if (values.size() < n)
            throw value_buffer_error{"not enough values to copy object"};

        auto out = Object{};
        auto bytes = std::as_writable_bytes(std::span{&out, 1});
        auto offset = std::size_t{0};
        for (auto chunk : values.first(n)) {
            auto chunk_bytes = std::as_bytes(chunk);
            std::memcpy(
                bytes.data() + offset,
                chunk_bytes.data(),
                chunk_bytes.size());
            offset += chunk_bytes.size();
        }
        return out;
    }

    value_storage<value_type> owned_buffer_{0};
    value_type * buffer_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t seek_ = 0;
    std::size_t size_ = 0;

    [[nodiscard]] std::size_t write_index() const noexcept
    {
        return detail::ring_write_index<value_type>(capacity_, seek_, size_);
    }

    void destroy_prefix(std::size_t n) noexcept
    {
        while (n != 0) {
            auto take = std::min(n, capacity_ - seek_);
            for (auto & value : std::span{buffer_ + seek_, take})
                std::destroy_at(&value);
            seek_ = (seek_ + take) % capacity_;
            size_ -= take;
            n -= take;
        }
    }
};

/// Source that repeatedly parses typed values from a byte reader.
template<typename T>
class byte_parser final : public value_source<T>
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = value_storage_ref<value_type>;
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
        storage_ref buffer)
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
    using storage_ref = value_storage_ref<value_type>;

    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Values, std::views::all_t<Range>>
    value_range_source(Range && values, storage_ref buffer)
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
    value_storage_ref<std::ranges::range_value_t<std::views::all_t<Range>>>)
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
