#pragma once

#include "nxtrt/buffer-core.hpp"
#include "nxtrt/land.hpp"
#include "nxtrt/deck.hpp"
#include "nxtrt/task/root.hpp"
#include "nxtrt/alloc_trace.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstring>
#include "nxtrt/format.hpp"
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

/// A feed or sink was misused or broke its protocol.
///
/// Examples: asking for more values than the buffer can hold, a cold path
/// that reports no progress or more progress than it made, writing into a
/// full @ref fixed_sink, or splatting move-only values.
struct value_buffer_error : buffer_error
{
    using buffer_error::buffer_error;
};

/// A feed ended before a verb that requires input got it.
///
/// Thrown by `take_one()`, `peek_one()`, `fill(n)`, `peek(n)`, `take(n)`,
/// `take_until()`, `expect()`, and by `take_struct()` when the end falls
/// inside an object.
struct value_end_of_stream : end_of_stream
{
    using end_of_stream::end_of_stream;
};

/// `feed::expect()` found a different value. Nothing was consumed.
struct unexpected_value : value_buffer_error
{
    using value_buffer_error::value_buffer_error;
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

/// Chunked view of a feed's or sink's buffered values (at most two spans).
template<typename T, std::size_t Inline = 2>
using value_chunks = buffer_chunks<T, Inline>;

/// Buffered asynchronous writer of `T` values, shaped like Zig's
/// `std.Io.Writer`.
///
/// The sink is its buffer. `write()` moves or copies values into a ring of
/// raw storage and returns a ready @ref nxtrt::hope "hope" as long as they
/// fit: no coroutine frame, no deck turn. Only when the buffer is full, or on
/// `flush()`, does the sink call its one virtual, `drain_more()`, which hands
/// staged values to the concrete destination (a file descriptor, a container,
/// a channel...). That call may suspend; its `hope` is then pending and
/// awaiting it runs the slow path as a task. See @ref rt_holding.
///
/// Values written are not delivered until they are drained. Call `flush()`
/// (or use @ref write_all) when the destination must see them. Destroying a
/// sink destroys its buffered values without draining them.
///
/// **Storage.** Construct with a `value_storage_ref` to borrow caller storage
/// (which must outlive the sink), or with a size to own a heap buffer. A zero
/// capacity is allowed: every write then goes straight to `drain_more()`. The
/// sink is neither copyable nor movable, so subclasses and borrowed views of
/// it may hold its address.
///
/// **Use.** A sink is not thread-safe and has one writer: do not start a
/// second operation while one is still pending. Spans returned by
/// `buffered()` and `unused_capacity()` are invalidated by the next write,
/// flush, or drain. Exceptions from `drain_more()` propagate out of the
/// awaited write or flush.
///
/// **Implementing a sink.** Override
/// `drain_more(value_chunk_view values, std::size_t splat)`:
///
/// - accept a nonempty prefix of the logical sequence `values`, in which the
///   last chunk is repeated `splat` times (`splat` is 1 for an ordinary
///   drain), and return how many values of that sequence were accepted;
/// - you may move from accepted values (they are destroyed afterwards) but
///   must leave the rest intact, because they stay buffered;
/// - returning zero, or more than the sequence holds, makes the base throw
///   @ref nxtrt::value_buffer_error "value_buffer_error". Use the protected `buffered_values()` and
///   `consume_buffered_for_derived()` only from subclasses that manage their
///   own staging.
template<typename T>
class sink
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = value_storage_ref<value_type>;
    using value_chunk_view = value_chunks<value_type>;
    using const_value_chunk_view = value_chunks<const value_type>;

    /// Borrow `buffer` as raw staging storage. It must outlive the sink and
    /// must not hold live objects the sink would overwrite.
    explicit sink(storage_ref buffer)
        : ring_(buffer.data, buffer.size)
    {}

    /// Allocate and own a staging buffer of `buffer_size` values.
    explicit sink(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , ring_(owned_buffer_.data(), owned_buffer_.size())
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
        return ring_.size();
    }

    [[nodiscard]] std::size_t unused_capacity_size() const noexcept
    {
        return ring_.unused_capacity_size();
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
    /// finishes. With zero capacity the value goes straight to `drain_more()`.
    hope<void> write(value_type value)
    {
        if (unused_capacity_size() != 0) {
            emplace_back(std::move(value));
            return hope<void>::ready();
        }

        return write_slow(std::move(value));
    }

    /// Copy a contiguous run of values into this sink.
    ///
    /// Ready when all of `values` fit in the free space. Otherwise the slow
    /// path drains as needed; a run at least as large as the buffer is
    /// flushed past it, drained from a temporary copy. `values` must stay
    /// valid until the returned hope completes.
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

    /// Copy a chunked run of values into this sink, chunk by chunk.
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

    /// Copy `pattern` repeated `splat` times into this sink.
    ///
    /// A repetition too large for the buffer is passed to `drain_more()` with
    /// a matching `splat` rather than expanded in memory. `pattern` must stay
    /// valid until the returned hope completes. Throws
    /// @ref nxtrt::value_buffer_error "value_buffer_error" if the total count overflows `std::size_t`.
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

    /// Drain all currently buffered values to the concrete destination.
    ///
    /// Ready when nothing is buffered. Calls `drain_more()` until the buffer
    /// is empty.
    hope<void> flush()
    {
        if (buffered_size() == 0) {
            reset_if_empty();
            return hope<void>::ready();
        }
        return flush_slow();
    }

    /// Make at least `capacity` contiguous free slots while keeping the
    /// newest `preserve` buffered values staged.
    ///
    /// Drains only the older values in front of the preserved suffix and
    /// moves buffered values to join split free space when necessary.
    /// Throws @ref nxtrt::value_buffer_error "value_buffer_error" up front
    /// when `preserve + capacity` exceeds the storage.
    hope<void> rebase(std::size_t preserve, std::size_t capacity)
    {
        require_preserved_capacity(preserve, capacity);
        if (unused_capacity().size() >= capacity)
            return hope<void>::ready();
        if (unused_capacity_size() >= capacity) {
            contiguize_buffered();
            return hope<void>::ready();
        }
        return rebase_slow(preserve, capacity);
    }

    /// Byte sinks: `rebase(preserve, minimum)`, then return all of the
    /// contiguous free space for the caller to write into.
    ///
    /// The bytes are not counted as buffered until the caller calls
    /// `advance_constructed()`.
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

    /// Byte sinks: reserve exactly `len` bytes of buffer and count them as
    /// written. The caller fills the returned span before the next flush.
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
        return ring_.capacity();
    }

    /// The contiguous run of free buffer slots after the buffered values.
    ///
    /// Producers (typically a feed's `stream_more()`) may write directly into
    /// it and then call `advance_constructed()`. For non-trivial `T` prefer
    /// `uninitialized_capacity()`, which does not claim the slots hold
    /// objects.
    [[nodiscard]] std::span<value_type> unused_capacity() noexcept
    {
        return ring_.unused_capacity();
    }

    /// Count `n` values just constructed at the front of
    /// `unused_capacity()` as buffered. Throws @ref nxtrt::value_buffer_error "value_buffer_error" if `n`
    /// exceeds that run.
    void advance_constructed(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw value_buffer_error{"value sink advanced past buffer capacity"};
        ring_.advance_constructed(n);
    }

    /// `unused_capacity()` as raw land in which to construct values.
    [[nodiscard]] junk<value_type> uninitialized_capacity() noexcept
    {
        auto span = unused_capacity();
        return {span.data(), span.size()};
    }

protected:
    [[nodiscard]] const_value_chunk_view buffered_chunks() const noexcept
    {
        return ring_.constructed();
    }

    [[nodiscard]] value_chunk_view buffered_values() noexcept
    {
        return ring_.constructed();
    }

    void release_buffered_without_destroying() noexcept
    {
        ring_.release_without_destroying();
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
    /// base sink. `values` may be this sink's own buffer or a temporary copy;
    /// it stays valid until the returned hope completes.
    virtual hope<std::size_t> drain_more(
        value_chunk_view values,
        std::size_t splat) = 0;

private:
    void emplace_back(value_type value)
    {
        if (unused_capacity_size() == 0)
            throw value_buffer_error{"value sink buffer is full"};
        std::construct_at(
            ring_.data() + ring_.write_index(),
            std::move(value));
        ring_.advance_constructed(1);
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
        ring_.advance_constructed(values.size());
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
        ring_.reset_if_empty();
    }

    void require_preserved_capacity(
        std::size_t preserve,
        std::size_t capacity) const
    {
        auto available = ring_.capacity();
        if (preserve > available || capacity > available - preserve)
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
        ring_.destroy_prefix(n);
        reset_if_empty();
    }

    void contiguize_buffered()
    {
        if (ring_.empty()) {
            ring_.reset_if_empty();
            return;
        }

        auto values = std::vector<value_type>{};
        values.reserve(ring_.size());
        for (auto chunk : buffered_values())
            for (auto & value : chunk)
                values.emplace_back(std::move(value));

        ring_.destroy_all();
        for (auto & value : values) {
            std::construct_at(
                ring_.data() + ring_.write_index(), std::move(value));
            ring_.advance_constructed(1);
        }
    }

    void destroy_buffered() noexcept
    {
        ring_.destroy_all();
    }

    task<void> flush_slow()
    {
        while (buffered_size() != 0)
            co_await drain_buffered_once();
    }

    task<void> write_slow(value_type value)
    {
        if (ring_.capacity() == 0) {
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
        if (ring_.capacity() != 0 && values.size() < ring_.capacity()) {
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
        if (ring_.capacity() == 0) {
            co_await write_splat_direct_slow(pattern, splat);
            co_return;
        }

        auto count = repeated_size(pattern.size(), splat);
        if (count < ring_.capacity()) {
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
            if (unused_capacity_size() >= capacity) {
                contiguize_buffered();
                co_return;
            }
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
        auto writable = std::span<value_type>{co_await std::move(slice)};
        auto out = writable.first(len);
        advance_constructed(len);
        co_return out;
    }

    rack<value_type> owned_buffer_{0};
    ring_region<value_type> ring_;
};

/// Sink that only buffers, into fixed caller-owned raw storage.
///
/// It has no destination: once the storage is full, any write that needs a
/// drain throws @ref nxtrt::value_buffer_error "value_buffer_error". Feeds use it to stream into their
/// own unused capacity or into caller spans. The destructor destroys the
/// buffered values unless `release_buffered()` hands them off first.
template<typename T>
class fixed_sink final : public sink<T>
{
public:
    using typename sink<T>::storage_ref;

    explicit fixed_sink(storage_ref buffer)
        : sink<T>(buffer)
    {}

    /// Forget the buffered values without destroying them, leaving their
    /// lifetime to whoever owns the storage.
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

/// Sink that accepts and destroys all values. Unbuffered by default.
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

/// Unbuffered sink that appends each value to a borrowed container.
///
/// Uses `push_back`, or `insert(end(), ...)`. With no buffer, each write
/// takes the slow path, but `drain_more()` never suspends: the value is in
/// the container once the write completes. The container must outlive the
/// sink. Splatting a move-only value type throws
/// @ref nxtrt::value_buffer_error "value_buffer_error".
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

/// Unbuffered sink that assigns each value through an output iterator.
///
/// Like @ref container_sink, `drain_more()` never suspends. `output()`
/// returns the advanced iterator.
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

/// Free-function spelling of `sink.write(...)`. Does not flush.
///
/// The `std::string_view` and `const char *` overloads for byte sinks write
/// the text's bytes; the `std::string` overload keeps the string alive in its
/// frame until the write completes.
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

/// Write, then flush, so the destination has seen everything when the task
/// completes.
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

/// Format with `std::format` syntax and write the text to `sink`. Does not
/// flush; `print_all` also flushes.
template<typename... Args>
task<void> print(
    sink<std::byte> & sink,
    std::format_string<Args...> fmt,
    Args &&... args)
{
    co_await write(sink, nxtrt::format(fmt, std::forward<Args>(args)...));
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
    auto text = nxtrt::format(fmt, std::forward<Args>(args)...);
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

/// Type-independent base of @ref nxtrt::feed "feed": the refill loops.
///
/// The loops only count buffered values and call `refill()`, so they
/// are compiled once (in buffers.cpp) instead of once per value type.
class feed_core
{
public:
    virtual ~feed_core() = default;

protected:
    /// Append whatever the source produces next to the buffer. Throws if
    /// the buffer is already full.
    virtual hope<fare_t> refill() = 0;
    [[nodiscard]] virtual std::size_t buffered_count() const noexcept = 0;

    /// Refill until at least `n` values are buffered; EOF first throws
    /// `value_end_of_stream`.
    task<void> fill_slow(std::size_t n);

    /// Refill until at least one value is buffered. Returns false at EOF.
    task<bool> fill_some_slow();

    /// As above, starting from a refill that is already in flight.
    task<bool> fill_some_slow(hope<fare_t> first_read);
};

/// Buffered asynchronous reader of `T` values, shaped like Zig's
/// `std.Io.Reader`.
///
/// The feed is its buffer: lookahead lives in a ring of storage owned or
/// borrowed by the feed itself, not in a wrapper. The reading verbs are
/// ordinary inline members that look at that buffer first. When the values
/// are already there they return a ready @ref nxtrt::hope "hope", so
/// `co_await feed.take()` on buffered data never suspends or touches the
/// deck. Only on a miss do they call the virtual cold path,
/// `stream_more()`, and return a pending hope that runs the refill as a
/// task. Layers that already hold their data (a decoder over a buffered
/// input, an in-memory span) answer the cold path with a ready hope too, so
/// whole stacks of feeds can read without suspending. See @ref rt_holding.
///
/// **Reading verbs.**
///
/// - `peek()` / `take()`: the next value, borrowed or consumed. They return
///   `nullptr` / `std::nullopt` at the end of the stream.
/// - `peek_one()` / `take_one()`: same, but the end throws
///   @ref value_end_of_stream.
/// - `fill(n)`, `peek(n)`: make `n` values visible without consuming them;
///   the end first throws @ref value_end_of_stream.
/// - `peek_struct<O>()` / `take_struct<O>()`: copy a trivially copyable
///   object out of the next values.
/// - `expect(v)`, `discard_all(vs...)`: consume values that must match, or
///   throw @ref unexpected_value without consuming.
/// - `stream(sink, limit)`: move up to `limit` values into a sink;
///   `discard(limit)`: drop them. Both move at most one chunk per call and
///   return a @ref fare_t count, or EOF. See @ref nxtrt::stream_all "stream_all".
/// - Byte feeds only: `take(n)`, `peek_span(n)`, `take_some(limit)`,
///   `take_string_view(n)`, `take_until(delimiter)`, `read(dst)`,
///   `read_vec(dsts)`.
///
/// A verb that needs `n` buffered values throws @ref nxtrt::value_buffer_error "value_buffer_error" when
/// `n` exceeds the buffer's capacity. Borrowed results (pointers, spans,
/// chunk views) point into the buffer and are invalidated by the next
/// operation that refills or consumes this feed. A taken span stays readable
/// until that next operation.
///
/// **Storage.** Construct with a `value_storage_ref` to borrow caller storage
/// (which must outlive the feed), or with a size to own a heap buffer.
/// Buffered values still present when the feed is destroyed are destroyed
/// with it. With zero capacity, `take()` still works (one value at a time,
/// through a temporary cell) but verbs that must buffer throw. Feeds are
/// neither copyable nor movable.
///
/// **Use.** A feed is not thread-safe and has one reader: do not start a
/// second operation while one is still pending. Exceptions thrown by the
/// cold path propagate out of the awaited verb.
///
/// **Implementing a source.** Override one of:
///
/// - `stream_more(sink, limit)`: deliver up to `limit` values into `sink`,
///   either with `sink.write(...)` or by constructing them in
///   `sink.uninitialized_capacity()` and calling `sink.advance_constructed()`.
///   Return the count, `eof` when the source has ended, or zero for "no
///   progress yet". Instead of writing to `sink` an implementation may
///   `emplace()` values into this feed's own buffer and return zero; the
///   next verb then finds them buffered. To refill its buffer the feed calls
///   `stream_more()` with a @ref fixed_sink over its own free space, so the
///   sink may be small and will throw if overfilled: respect
///   `sink.unused_capacity()`. A nonzero count must match what was written.
/// - `next_value()`: produce one value, or `std::nullopt` at the end. The
///   default `stream_more()` calls it and writes the value to the sink. It
///   is a task, so this path always goes through the deck.
///
/// `discard_more(limit)` may also be overridden to skip values without
/// materializing them; by default it streams into a @ref nxtrt::discarding_sink "discarding_sink".
template<typename T>
class feed : public feed_core
{
public:
    using value_type = std::remove_cv_t<T>;
    using storage_ref = value_storage_ref<value_type>;
    using value_chunk_view = value_chunks<value_type>;
    using const_value_chunk_view = value_chunks<const value_type>;

    /// Borrow `buffer` as raw lookahead storage. It must outlive the feed.
    explicit feed(storage_ref buffer)
        : ring_(buffer.data, buffer.size)
    {}

    /// Allocate and own a lookahead buffer of `buffer_size` values.
    explicit feed(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , ring_(owned_buffer_.data(), owned_buffer_.size())
    {}

    feed(const feed &) = delete;
    feed & operator=(const feed &) = delete;

    feed(feed &&) = delete;
    feed & operator=(feed &&) = delete;

    ~feed() override
    {
        destroy_buffered();
    }

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return ring_.size();
    }

    [[nodiscard]] std::size_t unused_capacity_size() const noexcept
    {
        return ring_.unused_capacity_size();
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
    /// `value_end_of_stream`. Throws `value_buffer_error` if `n` exceeds the
    /// buffer capacity.
    hope<void> fill(std::size_t n)
    {
        if (n > ring_.capacity())
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
            return hope<const value_type *>::ready(ring_.front_data());

        auto read = fill_more();
        if (!read.is_ready())
            return peek_slow(std::move(read));

        auto result = read.take_ready();
        if (buffered_size() != 0)
            return hope<const value_type *>::ready(ring_.front_data());
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
        if (n > ring_.capacity())
            throw value_buffer_error{"value source buffer is too small"};
        if (buffered_size() >= n)
            return hope<const_value_chunk_view>::ready(buffered().first(n));
        return peek_slow(n);
    }

    /// Copy a trivially copyable object from the next values without consuming.
    ///
    /// The object's size must be a whole number of values: for example, three
    /// buffered `int` values may be copied into a trivially copyable struct
    /// whose object representation is three ints wide. EOF before the whole
    /// object is buffered throws `value_end_of_stream`.
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
        if (ring_.capacity() == 0)
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

    /// Consume one expected value.
    ///
    /// On mismatch throws `unexpected_value` without consuming; at EOF throws
    /// `value_end_of_stream`.
    hope<void> expect(value_type expected)
        requires std::equality_comparable<value_type>
    {
        if (buffered_size() != 0) {
            expect_buffered(expected);
            return hope<void>::ready();
        }
        return expect_slow(std::move(expected));
    }

    /// `expect()` each argument in order. Values matched before a mismatch
    /// stay consumed.
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
    ///
    /// With values buffered, writes the first contiguous buffered chunk (up
    /// to `limit`) to `sink`, consuming it once the write completes
    /// (move-only values are moved out and consumed one by one). With an
    /// empty buffer, calls `stream_more()` directly, so the data can bypass
    /// this feed's buffer. Returns the count moved, zero when the source made
    /// no progress (or parked values in this feed's buffer), or EOF. Call in
    /// a loop until `is_eof()`, as @ref nxtrt::stream_all "stream_all" does.
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

    /// Discard up to `limit` values: buffered ones if there are any,
    /// otherwise through `discard_more()`. Returns the count or EOF, like
    /// `stream()`.
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

    /// Check that `capacity` fits the buffer, and copy wrapped buffered
    /// values into one contiguous run.
    ///
    /// It does not drain or guarantee `capacity` free slots; it throws
    /// `value_buffer_error` only when `capacity` exceeds the storage.
    void rebase(std::size_t capacity)
        requires std::is_trivially_copyable_v<value_type>
    {
        if (capacity > ring_.capacity())
            throw value_buffer_error{"value source buffer is too small"};
        contiguize_buffered_for_derived();
    }

    /// Byte feeds: the buffered bytes as one span. Throws
    /// `value_buffer_error` if they wrap around the ring.
    [[nodiscard]] std::span<const value_type> buffered_span() const
        requires std::same_as<value_type, std::byte>
    {
        auto one = buffered().single_span();
        if (!one)
            throw value_buffer_error{"value feed buffered values are wrapped"};
        return *one;
    }

    /// Byte feeds: `peek(n)`.
    hope<const_value_chunk_view> peek_chunks(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        return peek(n);
    }

    /// Byte feeds: borrow the next `n` bytes as one span without consuming
    /// them.
    ///
    /// Copies wrapped buffered bytes together when needed. EOF first throws
    /// `value_end_of_stream`; `n` above the capacity throws
    /// `value_buffer_error`.
    hope<std::span<const value_type>> peek_span(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        if (n > ring_.capacity())
            throw value_buffer_error{"value source buffer is too small"};
        if (buffered_size() >= n)
            return hope<std::span<const value_type>>::ready(
                contiguous_prefix(n));
        return peek_span_slow(n);
    }

    /// Byte feeds: consume exactly `n` bytes and borrow them as one span.
    ///
    /// Errors as for `peek_span()`. The span stays readable until the next
    /// operation on this feed.
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

    /// Byte feeds: consume and borrow whatever contiguous bytes are
    /// available, up to `limit`, refilling once if the buffer is empty.
    ///
    /// Returns `std::nullopt` at EOF. May return an empty span when a refill
    /// made no progress; call again.
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

    /// Byte feeds: `take(n)` viewed as text.
    hope<std::string_view> take_string_view(std::size_t n)
        requires std::same_as<value_type, std::byte>
    {
        auto bytes = take(n);
        if (bytes.is_ready())
            return hope<std::string_view>::ready(
                as_string_view(bytes.take_ready()));
        return take_string_view_slow(std::move(bytes));
    }

    /// Byte feeds: consume through the next `delimiter` and borrow the bytes
    /// before it.
    ///
    /// The delimiter is consumed but not returned. Throws
    /// `value_buffer_error` for an empty delimiter or when the buffer fills
    /// before one is found, and `value_end_of_stream` at EOF.
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

    /// Byte feeds: copy bytes into the caller's spans, in order.
    ///
    /// With bytes buffered, copies as many as fit without refilling. With an
    /// empty buffer, streams straight into the first nonempty span,
    /// bypassing this feed's buffer. Returns the count copied, or EOF; zero
    /// when every span is empty.
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

    /// Byte feeds: `read_vec()` with one destination.
    hope<fare_t> read(std::span<value_type> dst)
        requires std::same_as<value_type, std::byte>
    {
        auto dsts = std::array{dst};
        return read_vec(std::span{dsts});
    }

protected:
    [[nodiscard]] const_value_chunk_view buffered_chunks() const noexcept
    {
        return ring_.constructed();
    }

    [[nodiscard]] std::size_t storage_capacity() const noexcept
    {
        return ring_.capacity();
    }

    [[nodiscard]] std::span<value_type> buffer_storage() noexcept
    {
        return ring_.storage();
    }

    [[nodiscard]] std::span<value_type> unused_capacity() noexcept
    {
        return ring_.unused_capacity();
    }

    /// Append one value to this feed's own buffer, for a source that parks
    /// values locally instead of writing them to the sink it was given.
    /// Throws `value_buffer_error` when the buffer is full.
    void emplace(value_type value)
    {
        if (unused_capacity_size() == 0)
            throw value_buffer_error{"value source buffer is full"};
        std::construct_at(
            ring_.data() + ring_.write_index(),
            std::move(value));
        ring_.advance_constructed(1);
    }

    void advance_constructed(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw value_buffer_error{"value source advanced past buffer capacity"};
        ring_.advance_constructed(n);
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
        if (ring_.empty()) {
            ring_.reset_if_empty();
            return;
        }
        if (ring_.has_contiguous_constructed())
            return;

        auto values = std::vector<value_type>(ring_.size());
        auto offset = std::size_t{0};
        for (auto chunk : buffered_chunks()) {
            std::memcpy(
                values.data() + offset,
                chunk.data(),
                chunk.size_bytes());
            offset += chunk.size();
        }

        ring_.destroy_all();
        for (auto i = std::size_t{0}; i < values.size(); ++i)
            std::construct_at(ring_.data() + i, values[i]);
        ring_.advance_constructed(values.size());
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
    ///
    /// Return the next value, or `std::nullopt` at the end. The default
    /// throws `value_buffer_error`: a feed must override this or
    /// `stream_more()`.
    virtual task<std::optional<value_type>> next_value()
    {
        throw value_buffer_error{"value feed has no next implementation"};
        co_return std::nullopt;
    }

    /// Cold path for `discard()` when nothing is buffered. The default
    /// streams into a @ref nxtrt::discarding_sink "discarding_sink".
    virtual hope<fare_t> discard_more(std::size_t limit)
    {
        auto sink = discarding_sink<value_type>{};
        auto result = stream_more(sink, limit);
        if (result.is_ready())
            return hope<fare_t>::ready(result.take_ready());
        return discard_more_slow(limit);
    }

protected:
    /// Optional source-credit notification, after buffered values are consumed.
    /// A derived source may return admission capacity here, but must not refill:
    /// a span-taking caller may still be using the consumed bytes. Ordinary
    /// feeds pay no virtual call on the buffered hot path.
    void observe_consumption(
        void * context,
        void (*notify)(void *, std::size_t) noexcept) noexcept
    {
        consumption_context_ = context;
        consumption_notify_ = notify;
    }

private:
    void toss(std::size_t n)
    {
        if (n > buffered_size())
            throw value_buffer_error{"value source consumed past buffer"};
        ring_.destroy_prefix(n);
        reset_if_empty();
        if (consumption_notify_ != nullptr)
            consumption_notify_(consumption_context_, n);
    }

    std::optional<value_type> take_buffered()
    {
        auto value = std::optional<value_type>{
            std::move(*ring_.front_data()),
        };
        toss(1);
        return value;
    }

    void expect_buffered(const value_type & expected)
        requires std::equality_comparable<value_type>
    {
        if (!(*ring_.front_data() == expected))
            throw unexpected_value{"unexpected value"};
        toss(1);
    }

    void reset_if_empty() noexcept
    {
        ring_.reset_if_empty();
    }

    hope<fare_t> fill_more()
    {
        if (buffered_size() == ring_.capacity())
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
        ring_.advance_constructed(written);
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
        // A producer-backed feed may fill its ring between staging this
        // lazy refill and running it. In that case there is already data to
        // inspect.
        auto capacity = unused_capacity();
        if (capacity.empty())
            co_return 0;
        auto sink = fixed_sink<value_type>{
            capacity.first(std::min(limit, capacity.size())),
        };
        auto result = co_await stream_more(sink, limit);
        co_return finish_read(sink, result);
    }

    hope<fare_t> refill() final
    {
        return fill_more();
    }

    [[nodiscard]] std::size_t buffered_count() const noexcept final
    {
        return buffered_size();
    }

    task<const value_type *> peek_slow()
    {
        if (co_await fill_some_slow())
            co_return ring_.front_data();
        co_return nullptr;
    }

    task<const value_type *> peek_slow(hope<fare_t> first_read)
    {
        if (co_await fill_some_slow(std::move(first_read)))
            co_return ring_.front_data();
        co_return nullptr;
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
        if (co_await fill_some_slow())
            co_return take_buffered();
        co_return std::nullopt;
    }

    task<std::optional<value_type>> take_slow(hope<fare_t> first_read)
    {
        if (co_await fill_some_slow(std::move(first_read)))
            co_return take_buffered();
        co_return std::nullopt;
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
            auto value = std::move(*ring_.front_data());
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

            if (buffered_size() == ring_.capacity())
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
        ring_.destroy_all();
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
    ring_region<value_type> ring_;
    void * consumption_context_ = nullptr;
    void (*consumption_notify_)(void *, std::size_t) noexcept = nullptr;
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

/// Feed backed by a callable that reads values into raw storage.
///
/// `read(junk<T> dst)` must return a `task<fare_t>` or `task<std::size_t>`
/// that constructs up to `dst.size()` values at the front of `dst` and
/// reports how many. With `std::size_t`, zero means EOF; with @ref fare_t,
/// zero means no progress and `eof` means the end. Reporting more than
/// `dst.size()` throws @ref nxtrt::value_buffer_error "value_buffer_error". `T` must be trivially
/// copyable.
///
/// The callable writes straight into the destination sink's free space when
/// it has some (so a refill lands in this feed's buffer with no copy), and
/// otherwise into this feed's own buffer. The callable is stored by value; if
/// it is a lambda, its captures live as long as the feed. The default buffer
/// is 4096 values.
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

/// Feed of `Out` values parsed one at a time from a borrowed `feed<In>`.
///
/// `parser` is a plain function pointer taking the input feed and returning
/// `task<std::optional<Out>>`; `std::nullopt` ends this feed. Each value
/// costs one task, through the default `next_value()` path. The input feed
/// must outlive this one. A null parser throws @ref nxtrt::value_buffer_error "value_buffer_error". The
/// default buffer holds one value.
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

/// @ref function_parser_feed over a byte feed.
template<typename T>
using byte_parser = function_parser_feed<T, std::byte>;

/// Feed over a range or lazy view of values.
///
/// The range is wrapped with `std::views::all`, so an lvalue container is
/// borrowed (and must outlive the feed) while an rvalue is moved into the
/// feed as an owning view. Values are moved out with `iter_move` and the
/// range is traversed once. When writes are ready, `stream_more()` never
/// suspends, so taking from this feed needs no deck turn. The default
/// buffer holds one value.
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
///
/// Loops `source.stream(sink)` until EOF and returns the number of values
/// moved. Errors from either side propagate; the sink is not flushed then.
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
