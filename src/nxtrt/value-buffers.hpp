#pragma once

#include "nxtrt/buffer-core.hpp"
#include "nxtrt/deck.hpp"
#include "nxtrt/task.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

struct value_buffer_error : buffer_error
{
    using buffer_error::buffer_error;
};

struct value_end_of_stream : end_of_stream
{
    using end_of_stream::end_of_stream;
};

struct unexpected_value : value_buffer_error
{
    using value_buffer_error::value_buffer_error;
};

/// View of raw storage where up to `size()` values of `T` may be constructed.
///
/// Unlike `std::span<T>`, this does not claim that live `T` objects already
/// exist. Producers must start object lifetimes before reporting values as
/// constructed to a feed or sink.
template<typename T>
class junk
{
public:
    using value_type = std::remove_cv_t<T>;

    junk() = default;

    junk(value_type * data, std::size_t size)
        : data_(data)
        , size_(size)
    {}

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

namespace detail {

template<typename T, typename Result>
concept value_read_result =
    std::same_as<Result, fare_t>
    || std::same_as<Result, std::size_t>;

template<typename T, typename Read>
concept value_read_task =
    std::invocable<Read &, junk<T>>
    && is_task_v<std::invoke_result_t<Read &, junk<T>>>
    && value_read_result<
        T,
        task_result_t<std::invoke_result_t<Read &, junk<T>>>>;

template<typename T, typename Result>
fare_t normalize_value_read_result(Result result)
{
    if constexpr (std::same_as<Result, fare_t>) {
        return result;
    } else {
        if (result == 0)
            return eof;
        return result;
    }
}

} // namespace detail

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
class rack
{
public:
    using value_type = std::remove_cv_t<T>;

    explicit rack(std::size_t size)
        : data_(size == 0 ? nullptr : allocator_.allocate(size))
        , size_(size)
    {}

    rack(const rack &) = delete;
    rack & operator=(const rack &) = delete;

    rack(rack && other) noexcept
        : data_(std::exchange(other.data_, nullptr))
        , size_(std::exchange(other.size_, 0))
    {}

    rack & operator=(rack && other) noexcept
    {
        if (this != &other) {
            deallocate();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~rack()
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

/// Buffered asynchronous sink for typed values.
///
/// Accepted values are constructed in raw ring storage. The hot path is
/// non-virtual and ready when values fit; `drain_more()` is the cold path that
/// moves staged chunks into the concrete backend.
template<typename T>
class sink
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = value_storage_ref<value_type>;
    using value_chunk_view = value_chunks<value_type>;
    using const_value_chunk_view = value_chunks<const value_type>;

    explicit sink(storage_ref buffer)
        : buffer_(buffer.data)
        , capacity_(buffer.size)
    {}

    explicit sink(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_.data())
        , capacity_(owned_buffer_.size())
    {}

    sink(const sink &) = delete;
    sink & operator=(const sink &) = delete;

    sink(sink &&) = delete;
    sink & operator=(sink &&) = delete;

    virtual ~sink()
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

    hope<void> rebase(std::size_t preserve, std::size_t capacity)
    {
        require_preserved_capacity(preserve, capacity);
        if (unused_capacity().size() >= capacity)
            return hope<void>::ready();
        return rebase_slow(preserve, capacity);
    }

    hope<std::span<value_type>>
    writable_slice_greedy_preserve(
        std::size_t preserve,
        std::size_t minimum)
        requires std::same_as<value_type, std::byte>
    {
        auto ready = rebase(preserve, minimum);
        if (ready.is_ready())
            return hope<std::span<value_type>>::ready(unused_capacity());
        return writable_slice_greedy_preserve_slow(std::move(ready));
    }

    hope<std::span<value_type>>
    writable_slice_preserve(std::size_t preserve, std::size_t len)
        requires std::same_as<value_type, std::byte>
    {
        auto slice = writable_slice_greedy_preserve(preserve, len);
        if (slice.is_ready()) {
            auto out = slice.take_ready().first(len);
            advance_constructed(len);
            return hope<std::span<value_type>>::ready(out);
        }
        return writable_slice_preserve_slow(std::move(slice), len);
    }

    [[nodiscard]] std::size_t storage_capacity() const noexcept
    {
        return capacity_;
    }

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

    [[nodiscard]] junk<value_type> uninitialized_capacity() noexcept
    {
        auto span = unused_capacity();
        return {span.data(), span.size()};
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
    /// they accepted. When `splat` is greater than one, the last chunk is
    /// logically repeated that many times after all earlier chunks.
    /// Returning zero when any values are available is a protocol error for the
    /// base sink.
    virtual hope<std::size_t> drain_more(
        value_chunk_view values,
        std::size_t splat) = 0;

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

    void require_preserved_capacity(
        std::size_t preserve,
        std::size_t capacity) const
    {
        if (preserve > capacity_ || capacity > capacity_ - preserve)
            throw value_buffer_error{"value sink buffer is too small"};
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
            }, 1);
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
        if (capacity_ != 0 && values.size() < capacity_) {
            if (auto free = unused_capacity_size();
                free != 0 && values.size() > free) {
                append_to_buffer(values.first(free));
                values = values.subspan(free);
            }
            while (values.size() > unused_capacity_size())
                co_await drain_buffered_once();
            append_to_buffer(values);
            co_return;
        }

        co_await flush();
        co_await write_direct_slow(values);
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
        if (capacity_ == 0) {
            co_await write_splat_direct_slow(pattern, splat);
            co_return;
        }

        auto count = repeated_size(pattern.size(), splat);
        if (count < capacity_) {
            while (count > unused_capacity_size())
                co_await drain_buffered_once();
            append_splat_to_buffer(pattern, splat);
            co_return;
        }

        co_await flush();
        co_await write_splat_direct_slow(pattern, splat);
    }

    task<void> write_direct_slow(std::span<const value_type> values)
        requires std::copy_constructible<value_type>
    {
        auto copy = std::vector<value_type>{values.begin(), values.end()};
        auto rest = std::span{copy};
        while (!rest.empty()) {
            auto accepted = co_await drain_more(value_chunk_view{rest}, 1);
            require_progress(accepted, rest.size());
            rest = rest.subspan(accepted);
        }
    }

    task<void> write_splat_direct_slow(
        std::span<const value_type> pattern,
        std::size_t splat)
        requires std::copy_constructible<value_type>
    {
        auto values = std::vector<value_type>{pattern.begin(), pattern.end()};
        auto offset = std::size_t{0};
        while (splat != 0) {
            auto chunks = std::array{
                std::span<value_type>{},
                std::span<value_type>{},
            };
            auto count = std::size_t{0};
            auto effective_splat = splat;
            if (offset != 0) {
                chunks[count++] = std::span{values}.subspan(offset);
                --effective_splat;
            }
            if (effective_splat != 0)
                chunks[count++] = std::span{values};

            auto available =
                (offset == 0 ? 0 : values.size() - offset)
                + values.size() * effective_splat;
            auto accepted = co_await drain_more(
                value_chunk_view{std::span{chunks}.first(count)},
                effective_splat == 0 ? 1 : effective_splat);
            require_progress(accepted, available);

            if (offset != 0) {
                auto partial = values.size() - offset;
                if (accepted < partial) {
                    offset += accepted;
                    continue;
                }
                accepted -= partial;
                offset = 0;
                --splat;
            }

            auto whole = accepted / values.size();
            splat -= whole;
            auto rest = accepted % values.size();
            if (rest != 0) {
                offset = rest;
                --splat;
            }
        }
    }

    task<void> drain_buffered_once()
    {
        auto values = buffered_values();
        auto accepted = co_await drain_more(values, 1);
        require_progress(accepted, values.size());
        consume_buffered(accepted);
    }

    task<void> rebase_slow(std::size_t preserve, std::size_t capacity)
    {
        while (unused_capacity().size() < capacity) {
            auto values = buffered_values();
            auto drainable = values.size() - std::min(preserve, values.size());
            if (drainable == 0)
                throw value_buffer_error{"value sink buffer is too small"};

            auto prefix = values.first(drainable);
            auto accepted = co_await drain_more(prefix, 1);
            require_progress(accepted, drainable);
            consume_buffered(accepted);
        }
    }

    task<std::span<value_type>>
    writable_slice_greedy_preserve_slow(hope<void> ready)
    {
        co_await std::move(ready);
        co_return unused_capacity();
    }

    task<std::span<value_type>>
    writable_slice_preserve_slow(
        hope<std::span<value_type>> slice,
        std::size_t len)
    {
        auto out = (co_await std::move(slice)).first(len);
        advance_constructed(len);
        co_return out;
    }

    rack<value_type> owned_buffer_{0};
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
class fixed_sink final : public sink<T>
{
public:
    using typename sink<T>::storage_ref;

    explicit fixed_sink(storage_ref buffer)
        : sink<T>(buffer)
    {}

    void release_buffered() noexcept
    {
        this->release_buffered_without_destroying();
    }

private:
    hope<std::size_t>
    drain_more(
        typename sink<T>::value_chunk_view,
        std::size_t) override
    {
        throw value_buffer_error{"fixed value sink is full"};
    }
};

/// Sink that accepts and ignores all values.
template<typename T>
class discarding_sink final : public sink<T>
{
public:
    using typename sink<T>::storage_ref;

    explicit discarding_sink(storage_ref buffer = {})
        : sink<T>(buffer)
    {}

private:
    hope<std::size_t>
    drain_more(
        typename sink<T>::value_chunk_view values,
        std::size_t splat) override
    {
        return hope<std::size_t>::ready(splatted_size(values, splat));
    }

    static std::size_t splatted_size(
        typename sink<T>::value_chunk_view values,
        std::size_t splat)
    {
        if (values.empty())
            return 0;

        auto total = std::size_t{0};
        auto chunks = values.chunks();
        for (auto chunk : chunks.first(chunks.size() - 1))
            total += chunk.size();
        total += chunks.back().size() * splat;
        return total;
    }
};

/// Sink that appends accepted values directly into a container.
template<typename Container>
    requires detail::append_value_container<
        Container,
        typename Container::value_type>
class container_sink final
    : public sink<typename Container::value_type>
{
public:
    using value_type = typename Container::value_type;

    explicit container_sink(Container & container)
        : sink<value_type>(value_storage_ref<value_type>{})
        , container_(&container)
    {}

private:
    hope<std::size_t> drain_more(
        typename sink<value_type>::value_chunk_view values,
        std::size_t splat) override
    {
        auto total = std::size_t{0};
        auto move_chunk = [&](auto chunk) {
            for (auto & value : chunk)
                detail::append_value(*container_, std::move(value));
            total += chunk.size();
        };
        auto copy_chunk = [&](auto chunk) {
            if constexpr (std::copy_constructible<value_type>) {
                for (auto & value : chunk)
                    detail::append_value(*container_, value_type{value});
                total += chunk.size();
            } else {
                throw value_buffer_error{
                    "value sink cannot splat move-only values",
                };
            }
        };

        if (values.empty())
            return hope<std::size_t>::ready(0);

        auto chunks = values.chunks();
        for (auto chunk : chunks.first(chunks.size() - 1))
            move_chunk(chunk);
        if (splat == 1) {
            move_chunk(chunks.back());
        } else {
            for (auto i = std::size_t{0}; i < splat; ++i)
                copy_chunk(chunks.back());
        }
        return hope<std::size_t>::ready(total);
    }

    Container * container_;
};

template<typename Container>
container_sink(Container &) -> container_sink<Container>;

/// Sink that writes accepted values directly through an output iterator.
template<typename T, std::output_iterator<std::remove_cv_t<T>> Output>
class iterator_sink final : public sink<T>
{
public:
    using value_type = std::remove_cv_t<T>;

    explicit iterator_sink(Output output)
        : sink<T>(value_storage_ref<value_type>{})
        , output_(std::move(output))
    {}

    [[nodiscard]] Output output() const
    {
        return output_;
    }

private:
    hope<std::size_t> drain_more(
        typename sink<T>::value_chunk_view values,
        std::size_t splat) override
    {
        auto total = std::size_t{0};
        auto move_chunk = [&](auto chunk) {
            for (auto & value : chunk) {
                *output_ = std::move(value);
                ++output_;
            }
            total += chunk.size();
        };
        auto copy_chunk = [&](auto chunk) {
            if constexpr (std::copy_constructible<value_type>) {
                for (auto & value : chunk) {
                    *output_ = value_type{value};
                    ++output_;
                }
                total += chunk.size();
            } else {
                throw value_buffer_error{
                    "value sink cannot splat move-only values",
                };
            }
        };

        if (values.empty())
            return hope<std::size_t>::ready(0);

        auto chunks = values.chunks();
        for (auto chunk : chunks.first(chunks.size() - 1))
            move_chunk(chunk);
        if (splat == 1) {
            move_chunk(chunks.back());
        } else {
            for (auto i = std::size_t{0}; i < splat; ++i)
                copy_chunk(chunks.back());
        }
        return hope<std::size_t>::ready(total);
    }

    Output output_;
};

/// Write values and flush the sink.
template<typename T>
hope<void> write(sink<T> & sink, std::remove_cv_t<T> value)
{
    return sink.write(std::move(value));
}

template<typename T>
hope<void> write(
    sink<T> & sink,
    std::span<const std::remove_cv_t<T>> values)
    requires std::copy_constructible<std::remove_cv_t<T>>
{
    return sink.write(values);
}

template<typename T, std::size_t Inline>
hope<void> write(
    sink<T> & sink,
    value_chunks<const std::remove_cv_t<T>, Inline> values)
    requires std::copy_constructible<std::remove_cv_t<T>>
{
    return sink.write(values);
}

template<typename T>
hope<void> write_splat(
    sink<T> & sink,
    std::span<const std::remove_cv_t<T>> pattern,
    std::size_t splat)
    requires std::copy_constructible<std::remove_cv_t<T>>
{
    return sink.write_splat(pattern, splat);
}

template<typename T>
task<void> write_all(sink<T> & sink, std::remove_cv_t<T> value)
{
    co_await sink.write(std::move(value));
    co_await sink.flush();
}

template<typename T>
task<void> write_all(
    sink<T> & sink,
    std::span<const std::remove_cv_t<T>> values)
    requires std::copy_constructible<std::remove_cv_t<T>>
{
    co_await sink.write(values);
    co_await sink.flush();
}

template<typename T, std::size_t Inline>
task<void> write_all(
    sink<T> & sink,
    value_chunks<const std::remove_cv_t<T>, Inline> values)
    requires std::copy_constructible<std::remove_cv_t<T>>
{
    co_await sink.write(values);
    co_await sink.flush();
}

inline hope<void> write(
    sink<std::byte> & sink,
    std::string_view text)
{
    return sink.write(as_bytes(text));
}

inline hope<void> write(sink<std::byte> & sink, const char * text)
{
    return write(sink, std::string_view{text});
}

inline task<void> write(sink<std::byte> & sink, std::string text)
{
    co_await write(sink, std::string_view{text});
}

inline hope<void> write_splat(
    sink<std::byte> & sink,
    std::string_view pattern,
    std::size_t splat)
{
    return sink.write_splat(as_bytes(pattern), splat);
}

inline task<void> write_all(
    sink<std::byte> & sink,
    std::string_view text)
{
    co_await write(sink, text);
    co_await sink.flush();
}

inline task<void> write_all(
    sink<std::byte> & sink,
    std::string text)
{
    co_await write_all(sink, std::string_view{text});
}

template<typename... Args>
task<void> print(
    sink<std::byte> & sink,
    std::format_string<Args...> fmt,
    Args &&... args)
{
    co_await write(sink, std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
task<void> print_all(
    sink<std::byte> & sink,
    std::format_string<Args...> fmt,
    Args &&... args)
{
    co_await print(sink, fmt, std::forward<Args>(args)...);
    co_await sink.flush();
}

template<typename... Args>
task<void> print(
    sink<char> & sink,
    std::format_string<Args...> fmt,
    Args &&... args)
{
    auto text = std::format(fmt, std::forward<Args>(args)...);
    co_await sink.write(std::span<const char>{text});
}

template<typename... Args>
task<void> print_all(
    sink<char> & sink,
    std::format_string<Args...> fmt,
    Args &&... args)
{
    co_await print(sink, fmt, std::forward<Args>(args)...);
    co_await sink.flush();
}

/// Buffered asynchronous feed for typed values.
///
/// Lookahead lives in reusable ring storage. The hot path borrows or consumes
/// buffered values directly; `stream_more()` is the cold path that produces more
/// values from the concrete source.
template<typename T>
class feed
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = value_storage_ref<value_type>;
    using value_chunk_view = value_chunks<value_type>;
    using const_value_chunk_view = value_chunks<const value_type>;

    explicit feed(storage_ref buffer)
        : buffer_(buffer.data)
        , capacity_(buffer.size)
    {}

    explicit feed(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_.data())
        , capacity_(owned_buffer_.size())
    {}

    feed(const feed &) = delete;
    feed & operator=(const feed &) = delete;

    feed(feed &&) = delete;
    feed & operator=(feed &&) = delete;

    virtual ~feed()
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
        if (is_eof(result) && value_count(result) == 0)
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
        if (capacity_ == 0)
            return take_direct_slow();

        auto read = fill_more();
        if (!read.is_ready())
            return take_slow(std::move(read));

        auto result = read.take_ready();
        if (buffered_size() != 0)
            return hope<std::optional<value_type>>::ready(take_buffered());
        if (is_eof(result) && value_count(result) == 0)
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
    hope<fare_t> stream(
        sink<value_type> & sink,
        std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (limit == 0)
            return hope<fare_t>::ready(0);

        if (buffered_size() == 0)
            return stream_more(sink, limit);

        return stream_buffered(sink, limit);
    }

    /// Discard one available chunk, up to `limit` values.
    hope<fare_t> discard(
        std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (limit == 0)
            return hope<fare_t>::ready(0);

        if (buffered_size() == 0)
            return discard_more(limit);

        auto n = std::min(limit, buffered_size());
        toss(n);
        return hope<fare_t>::ready(n);
    }

    void rebase(std::size_t capacity)
        requires std::is_trivially_copyable_v<value_type>
    {
        if (capacity > capacity_)
            throw value_buffer_error{"value source buffer is too small"};
        contiguize_buffered_for_derived();
    }

    [[nodiscard]] std::span<const value_type> buffered_span() const
        requires std::same_as<value_type, std::byte>
    {
        auto one = buffered().single_span();
        if (!one)
            throw value_buffer_error{"value feed buffered values are wrapped"};
        return *one;
    }

    hope<const_value_chunk_view> peek_chunks(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        return peek(n);
    }

    hope<std::span<const value_type>> peek_span(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        if (n > capacity_)
            throw value_buffer_error{"value source buffer is too small"};
        if (buffered_size() >= n)
            return hope<std::span<const value_type>>::ready(
                contiguous_prefix(n));
        return peek_span_slow(n);
    }

    hope<std::span<const value_type>> take(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        auto values = peek_span(n);
        if (values.is_ready()) {
            auto out = values.take_ready();
            toss(n);
            return hope<std::span<const value_type>>::ready(out);
        }
        return take_span_slow(std::move(values), n);
    }

    hope<std::optional<std::span<const value_type>>>
    take_some(std::size_t limit = std::numeric_limits<std::size_t>::max())
        requires std::same_as<value_type, std::byte>
    {
        if (buffered_size() == 0) {
            auto read = fill_more();
            if (!read.is_ready())
                return take_some_span_slow(std::move(read), limit);
            auto result = read.take_ready();
            if (is_eof(result) && value_count(result) == 0)
                return hope<std::optional<std::span<const value_type>>>::
                    ready(std::nullopt);
            if (value_count(result) == 0)
                return hope<std::optional<std::span<const value_type>>>::
                    ready(buffered_span().first(0));
        }

        auto out = first_buffered_span(limit);
        toss(out.size());
        return hope<std::optional<std::span<const value_type>>>::ready(out);
    }

    hope<std::string_view> take_string_view(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        auto bytes = take(n);
        if (bytes.is_ready())
            return hope<std::string_view>::ready(
                as_string_view(bytes.take_ready()));
        return take_string_view_slow(std::move(bytes));
    }

    hope<std::span<const value_type>> take_until(
        std::span<const value_type> delimiter)
        requires std::same_as<value_type, std::byte>
    {
        if (delimiter.empty())
            throw value_buffer_error{"empty delimiter"};

        contiguize_buffered_for_derived();
        auto available = buffered_span();
        auto cut = find_bytes(available, delimiter);
        if (cut < available.size()) {
            auto out = available.first(cut);
            toss(cut + delimiter.size());
            return hope<std::span<const value_type>>::ready(out);
        }

        return take_until_slow(delimiter);
    }

    hope<std::span<const value_type>> take_until(std::string_view delimiter)
        requires std::same_as<value_type, std::byte>
    {
        return take_until(as_bytes(delimiter));
    }

    hope<fare_t> read_vec(std::span<std::span<value_type>> dsts)
        requires std::same_as<value_type, std::byte>
    {
        if (buffered_size() != 0) {
            auto n = copy_buffered_to(dsts);
            return hope<fare_t>::ready(n);
        }

        auto dst = std::span<value_type>{};
        for (auto candidate : dsts) {
            if (!candidate.empty()) {
                dst = candidate;
                break;
            }
        }
        if (dst.empty())
            return hope<fare_t>::ready(0);

        auto out = fixed_sink<value_type>{dst};
        auto result = stream_more(out, dst.size());
        if (result.is_ready())
            return hope<fare_t>::ready(
                finish_direct_read(out, result.take_ready()));
        return read_vec_slow(dst);
    }

    hope<fare_t> read(std::span<value_type> dst)
        requires std::same_as<value_type, std::byte>
    {
        auto dsts = std::array{dst};
        return read_vec(std::span{dsts});
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

    [[nodiscard]] junk<value_type> uninitialized_capacity() noexcept
    {
        auto span = unused_capacity();
        return {span.data(), span.size()};
    }

    void consume_buffered_for_derived(std::size_t n)
    {
        toss(n);
    }

    std::optional<value_type> take_buffered_for_derived()
    {
        if (buffered_size() == 0)
            return std::nullopt;
        return take_buffered();
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

    /// Cold-path source operation.
    ///
    /// Implementations that can produce many values at once should transfer up
    /// to `limit` values from the underlying source into `sink`, returning how
    /// many values were logically advanced. A zero value count does not by
    /// itself mean EOF.
    ///
    /// Like `feed<std::byte>::stream_more()`, an implementation may instead append
    /// values to this source's own buffer with `emplace()` and return zero.
    /// That is the fallback when the destination cannot immediately accept a
    /// value and the source has local buffer space.
    virtual hope<fare_t> stream_more(
        sink<value_type> & sink,
        std::size_t limit)
    {
        if (limit == 0)
            return hope<fare_t>::ready(0);
        return stream_next(sink);
    }

    /// Single-value source hook used by the default `stream_more()`.
    virtual task<std::optional<value_type>> next_value()
    {
        throw value_buffer_error{"value feed has no next implementation"};
        co_return std::nullopt;
    }

    virtual hope<fare_t> discard_more(std::size_t limit)
    {
        auto sink = discarding_sink<value_type>{};
        auto result = stream_more(sink, limit);
        if (result.is_ready())
            return hope<fare_t>::ready(result.take_ready());
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

    hope<fare_t> fill_more()
    {
        if (buffered_size() == capacity_)
            throw value_buffer_error{"value source buffer is full"};
        return fill_more_into_capacity();
    }

    hope<fare_t> fill_more_into_capacity()
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw value_buffer_error{"value source buffer is full"};

        auto sink = fixed_sink<value_type>{dst};
        auto result = stream_more(sink, dst.size());
        if (result.is_ready())
            return hope<fare_t>::ready(
                finish_read(sink, result.take_ready()));
        return read_more_slow(dst.size());
    }

    fare_t finish_read(
        fixed_sink<value_type> & sink,
        fare_t result)
    {
        auto written = sink.buffered_size();
        auto reported = value_count(result);
        if (reported != 0 && written != reported)
            throw value_buffer_error{"value source stream count mismatch"};
        sink.release_buffered();
        size_ += written;
        if (written == 0)
            return result;
        return written;
    }

    fare_t finish_direct_read(
        fixed_sink<value_type> & sink,
        fare_t result)
        requires std::same_as<value_type, std::byte>
    {
        auto written = sink.buffered_size();
        auto reported = value_count(result);
        if (reported != 0 && written != reported)
            throw value_buffer_error{"value source stream count mismatch"};
        sink.release_buffered();
        if (written == 0)
            return result;
        return written;
    }

    task<fare_t> read_more_slow(std::size_t limit)
    {
        auto sink = fixed_sink<value_type>{
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
            if (is_eof(read) && value_count(read) == 0 && buffered_size() == before)
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
            if (is_eof(read) && value_count(read) == 0 && buffered_size() == before)
                co_return nullptr;
        }
    }

    task<const value_type *> peek_slow(hope<fare_t> first_read)
    {
        auto before = buffered_size();
        auto read = co_await std::move(first_read);
        if (buffered_size() != 0)
            co_return buffer_ + seek_;
        if (is_eof(read) && value_count(read) == 0 && buffered_size() == before)
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
            if (is_eof(read) && value_count(read) == 0 && buffered_size() == before)
                co_return std::nullopt;
        }
    }

    task<std::optional<value_type>> take_slow(hope<fare_t> first_read)
    {
        auto before = buffered_size();
        auto read = co_await std::move(first_read);
        if (buffered_size() != 0)
            co_return take_buffered();
        if (is_eof(read) && value_count(read) == 0 && buffered_size() == before)
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
                    is_eof(read)
                    && value_count(read) == 0
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

    hope<fare_t> stream_buffered(
        sink<value_type> & sink,
        std::size_t limit)
    {
        auto n = std::min(limit, buffered_size());
        if constexpr (std::copy_constructible<value_type>) {
            auto chunk = buffered().first(n).chunks().front();
            auto write = sink.write(chunk);
            if (!write.is_ready())
                return stream_buffered_slow(
                    std::move(write),
                    chunk.size(),
                    true);
            toss(chunk.size());
            return hope<fare_t>::ready(
                chunk.size());
        }

        auto moved = std::size_t{0};
        while (moved != n) {
            auto value = std::move(buffer_[seek_]);
            toss(1);
            auto write = sink.write(std::move(value));
            ++moved;
            if (!write.is_ready())
                return stream_buffered_slow(std::move(write), moved, false);
        }

        return hope<fare_t>::ready(moved);
    }

    task<fare_t> stream_buffered_slow(
        hope<void> write,
        std::size_t moved,
        bool consume_after)
    {
        co_await std::move(write);
        if (consume_after)
            toss(moved);
        co_return moved;
    }

    task<fare_t> discard_more_slow(std::size_t limit)
    {
        auto sink = discarding_sink<value_type>{};
        co_return co_await stream_more(sink, limit);
    }

    task<std::optional<value_type>> take_direct_slow()
    {
        while (true) {
            auto storage = rack<value_type>{1};
            auto out = fixed_sink<value_type>{storage};
            auto result = co_await stream_more(out, 1);
            auto written = out.buffered_size();
            auto reported = value_count(result);
            if (reported != 0 && written != reported)
                throw value_buffer_error{"value source stream count mismatch"};
            if (written != 0)
                co_return std::optional<value_type>{std::move(storage.data()[0])};
            if (is_eof(result))
                co_return std::nullopt;
        }
    }

    task<fare_t> stream_next(sink<value_type> & sink)
    {
        auto value = co_await next_value();
        if (!value)
            co_return eof;

        co_await sink.write(std::move(*value));
        co_return 1;
    }

    std::span<const value_type> first_buffered_span(
        std::size_t limit = std::numeric_limits<std::size_t>::max()) const
        requires std::same_as<value_type, std::byte>
    {
        auto chunks = buffered();
        auto first = chunks.single_span();
        if (first)
            return first->first(std::min(limit, first->size()));
        return chunks.chunks().front().first(
            std::min(limit, chunks.chunks().front().size()));
    }

    std::span<const value_type> contiguous_prefix(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        auto chunks = buffered().first(n);
        auto one = chunks.single_span();
        if (one)
            return *one;
        contiguize_buffered_for_derived();
        return buffered_span().first(n);
    }

    std::size_t copy_buffered_to(std::span<std::span<value_type>> dsts)
        requires std::same_as<value_type, std::byte>
    {
        auto total = std::size_t{0};
        for (auto dst : dsts) {
            if (dst.empty())
                continue;
            auto n = std::min(dst.size(), buffered_size());
            if (n == 0)
                break;
            auto src = first_buffered_span(n);
            n = src.size();
            std::memcpy(dst.data(), src.data(), n);
            toss(n);
            total += n;
        }
        return total;
    }

    task<std::span<const value_type>> peek_span_slow(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        co_await fill(n);
        co_return contiguous_prefix(n);
    }

    task<std::span<const value_type>> take_span_slow(
        hope<std::span<const value_type>> values,
        std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        auto out = co_await std::move(values);
        toss(n);
        co_return out;
    }

    task<std::optional<std::span<const value_type>>>
    take_some_span_slow(hope<fare_t> first_read, std::size_t limit)
        requires std::same_as<value_type, std::byte>
    {
        auto read = co_await std::move(first_read);
        if (is_eof(read) && value_count(read) == 0)
            co_return std::nullopt;
        if (value_count(read) == 0)
            co_return buffered_span().first(0);

        auto out = first_buffered_span(limit);
        toss(out.size());
        co_return out;
    }

    task<std::string_view> take_string_view_slow(
        hope<std::span<const value_type>> bytes)
        requires std::same_as<value_type, std::byte>
    {
        co_return as_string_view(co_await std::move(bytes));
    }

    task<std::span<const value_type>> take_until_slow(
        std::span<const value_type> delimiter)
        requires std::same_as<value_type, std::byte>
    {
        while (true) {
            contiguize_buffered_for_derived();
            auto available = buffered_span();
            auto cut = find_bytes(available, delimiter);
            if (cut < available.size()) {
                auto out = available.first(cut);
                toss(cut + delimiter.size());
                co_return out;
            }

            if (buffered_size() == capacity_)
                throw value_buffer_error{"value source buffer filled before delimiter"};

            auto read = co_await fill_more();
            if (is_eof(read) && value_count(read) == 0)
                throw value_end_of_stream{"unexpected end of value input"};
        }
    }

    task<fare_t> read_vec_slow(std::span<value_type> dst)
        requires std::same_as<value_type, std::byte>
    {
        auto out = fixed_sink<value_type>{dst};
        auto result = co_await stream_more(out, dst.size());
        co_return finish_direct_read(out, result);
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

    rack<value_type> owned_buffer_{0};
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

namespace detail {

template<typename T, typename Derived>
class taskfeed_base : public feed<T>
{
public:
    using value_type = typename feed<T>::value_type;
    using storage_ref = typename feed<T>::storage_ref;

    explicit taskfeed_base(storage_ref buffer)
        : feed<T>(buffer)
    {}

    explicit taskfeed_base(std::size_t buffer_size)
        : feed<T>(buffer_size)
    {}

private:
    hope<fare_t> stream_more(
        sink<value_type> & sink,
        std::size_t limit) override
    {
        return stream_more_task(sink, limit);
    }

    task<fare_t> stream_more_task(
        sink<value_type> & sink,
        std::size_t limit)
    {
        if (limit == 0)
            co_return 0;

        auto dst = sink.uninitialized_capacity();
        if (dst.empty()) {
            this->rebase(1);
            dst = this->uninitialized_capacity().first(limit);
            auto out = co_await read_into(dst);
            this->advance_constructed(value_count(out));
            if (is_eof(out) && value_count(out) == 0)
                co_return eof;
            co_return std::size_t{0};
        }

        auto out = co_await read_into(dst.first(limit));
        sink.advance_constructed(value_count(out));
        co_return out;
    }

    task<fare_t> read_into(junk<value_type> dst)
    {
        auto result = co_await derived().read_into(dst);
        auto out = normalize_value_read_result<value_type>(result);
        if (value_count(out) > dst.size())
            throw value_buffer_error{"source overfilled taskfeed buffer"};
        co_return out;
    }

    Derived & derived() noexcept
    {
        return static_cast<Derived &>(*this);
    }
};

} // namespace detail

/// Feed backed by a task callable that constructs values into raw storage.
///
/// The callable receives `junk<T>` and reports how many values it constructed.
/// A count-only result treats zero values as EOF.
template<typename T, typename Read>
    requires std::is_trivially_copyable_v<std::remove_cv_t<T>>
        && detail::value_read_task<std::remove_cv_t<T>, Read>
class taskfeed
    : public detail::taskfeed_base<std::remove_cv_t<T>, taskfeed<T, Read>>
{
    using base =
        detail::taskfeed_base<std::remove_cv_t<T>, taskfeed<T, Read>>;

public:
    using value_type = std::remove_cv_t<T>;
    using typename base::storage_ref;

    taskfeed(Read read, storage_ref buffer)
        : base(buffer)
        , read_(std::move(read))
    {}

    template<std::size_t Extent>
    taskfeed(Read read, std::span<value_type, Extent> buffer)
        : taskfeed(std::move(read), storage_ref{std::span<value_type>{buffer}})
    {}

    explicit taskfeed(Read read, std::size_t buffer_size = 4096)
        : base(buffer_size)
        , read_(std::move(read))
    {}

    auto read_into(junk<value_type> dst)
    {
        return std::invoke(read_, dst);
    }

private:
    friend base;

    Read read_;
};

template<typename Read, typename T, std::size_t Extent>
taskfeed(Read, std::span<T, Extent>) -> taskfeed<T, Read>;

template<typename Read, typename T>
taskfeed(Read, value_storage_ref<T>) -> taskfeed<T, Read>;

/// Parser feed backed by a callable parser function.
template<typename Out, typename In>
class function_parser_feed final : public feed<Out>
{
public:
    using base = feed<Out>;
    using value_type = typename base::value_type;
    using input_type = std::remove_cv_t<In>;
    using storage_ref = typename base::storage_ref;
    using parser_type = task<std::optional<value_type>> (*)(feed<input_type> &);

    function_parser_feed(
        feed<input_type> & input,
        parser_type parser,
        std::size_t buffer_size = 1)
        : base(buffer_size)
        , input_(&input)
        , parser_(parser)
    {
        if (parser_ == nullptr)
            throw value_buffer_error{"parser function is null"};
    }

    function_parser_feed(
        feed<input_type> & input,
        parser_type parser,
        storage_ref buffer)
        : base(buffer)
        , input_(&input)
        , parser_(parser)
    {
        if (parser_ == nullptr)
            throw value_buffer_error{"parser function is null"};
    }

private:
    task<std::optional<value_type>> next_value() override
    {
        co_return co_await parser_(*input_);
    }

    feed<input_type> * input_;
    parser_type parser_;
};

/// Source that repeatedly parses typed values from a byte feed.
template<typename T>
using byte_parser = function_parser_feed<T, std::byte>;

/// Source backed by a single-pass range or lazy view of values.
template<std::ranges::input_range Values>
    requires std::ranges::view<Values>
        && std::constructible_from<
            std::remove_cv_t<std::ranges::range_value_t<Values>>,
            std::ranges::range_rvalue_reference_t<Values>>
class value_range_source final
    : public feed<std::ranges::range_value_t<Values>>
{
public:
    using value_type =
        std::remove_cv_t<std::ranges::range_value_t<Values>>;
    using storage_ref = value_storage_ref<value_type>;

    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Values, std::views::all_t<Range>>
    value_range_source(Range && values, storage_ref buffer)
        : feed<value_type>(buffer)
        , values_(std::views::all(std::forward<Range>(values)))
        , value_(std::ranges::begin(values_))
        , end_(std::ranges::end(values_))
    {}

    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Values, std::views::all_t<Range>>
    explicit value_range_source(
        Range && values,
        std::size_t buffer_size = 1)
        : feed<value_type>(buffer_size)
        , values_(std::views::all(std::forward<Range>(values)))
        , value_(std::ranges::begin(values_))
        , end_(std::ranges::end(values_))
    {}

private:
    hope<fare_t> stream_more(
        sink<value_type> & sink,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<fare_t>::ready(0);

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

        if (total == 0 && value_ == end_)
            return hope<fare_t>::ready(eof);
        return hope<fare_t>::ready(total);
    }

    task<fare_t> stream_write_slow(
        hope<void> write,
        std::size_t prefix)
    {
        co_await std::move(write);
        ++value_;
        co_return prefix + 1;
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
    feed<T> & source,
    sink<std::remove_cv_t<T>> & sink)
{
    auto total = std::size_t{0};
    while (true) {
        auto result = co_await source.stream(sink);
        total += value_count(result);
        if (is_eof(result))
            break;
    }
    co_await sink.flush();
    co_return total;
}

extern template class sink<std::byte>;
extern template class feed<std::byte>;
extern template class fixed_sink<std::byte>;
extern template class discarding_sink<std::byte>;

} // namespace nxtrt
