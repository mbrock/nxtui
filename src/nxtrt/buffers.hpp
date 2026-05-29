#pragma once

#include "nxtrt/task.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

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
inline std::string_view as_string_view(std::span<const std::byte> bytes) noexcept
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
    std::span<const std::byte> haystack,
    std::span<const std::byte> needle)
{
    auto match = std::ranges::search(haystack, needle);
    return static_cast<std::size_t>(
        std::distance(haystack.begin(), match.begin()));
}

struct read_result
{
    /// Bytes written into the requested destination.
    ///
    /// Zero bytes is valid progresslessness; it only means EOF when `eof` is
    /// also true.
    std::size_t bytes = 0;
    /// True when the source is known to be exhausted.
    bool eof = false;
};

template<typename Read>
concept byte_read_task =
    std::invocable<Read &, std::span<std::byte>>
    && is_task_v<std::invoke_result_t<Read &, std::span<std::byte>>>
    && (
        std::same_as<
            task_result_t<std::invoke_result_t<Read &, std::span<std::byte>>>,
            read_result>
        || std::same_as<
            task_result_t<std::invoke_result_t<Read &, std::span<std::byte>>>,
            std::size_t>);

inline task<std::size_t> send_some(
    int fd,
    std::span<const std::byte> buffer,
    int flags = 0)
{
    co_return co_await op::send_some{
        .fd = fd,
        .buffer = buffer,
        .flags = flags,
    };
}

inline task<std::size_t> write_some(
    int fd,
    std::span<const std::byte> buffer,
    off_t offset = -1)
{
    co_return co_await op::write_some{
        .fd = fd,
        .buffer = buffer,
        .offset = offset,
    };
}

namespace detail {

template<typename T>
concept byte_writer_chunk =
    std::same_as<std::remove_cvref_t<T>, std::string>
    || std::convertible_to<T, std::string_view>
    || std::convertible_to<T, std::span<const std::byte>>;

template<typename T>
concept byte_writer_chunk_range =
    std::ranges::input_range<T>
    && (!byte_writer_chunk<T>)
    && byte_writer_chunk<std::ranges::range_reference_t<T>>;

template<typename T>
concept byte_reader_chunk =
    std::convertible_to<T, std::span<const std::byte>>
    || (
        std::convertible_to<T, std::string_view>
        && (
            !std::same_as<std::remove_cvref_t<T>, std::string>
            || std::is_lvalue_reference_v<T>));

template<typename T>
concept byte_reader_chunk_range =
    std::ranges::input_range<T>
    && byte_reader_chunk<std::ranges::range_reference_t<T>>;

template<byte_reader_chunk Chunk>
std::span<const std::byte> reader_chunk_bytes(Chunk && chunk) noexcept
{
    if constexpr (std::convertible_to<Chunk, std::span<const std::byte>>) {
        return std::span<const std::byte>{std::forward<Chunk>(chunk)};
    } else {
        return as_bytes(std::string_view{std::forward<Chunk>(chunk)});
    }
}

inline std::size_t byte_size(
    std::span<const std::span<const std::byte>> chunks) noexcept
{
    auto total = std::size_t{0};
    for (auto chunk : chunks)
        total += chunk.size();
    return total;
}

inline std::size_t byte_size(
    std::span<const std::span<const std::byte>> chunks,
    std::size_t splat)
{
    if (chunks.empty())
        return 0;

    auto total = std::size_t{0};
    for (auto chunk : chunks.first(chunks.size() - 1))
        total += chunk.size();

    auto last = chunks.back().size();
    if (last != 0 && splat > std::numeric_limits<std::size_t>::max() / last)
        throw buffer_error{"byte count overflow"};
    return total + last * splat;
}

inline std::span<const std::byte> first_nonempty(
    std::span<const std::span<const std::byte>> chunks) noexcept
{
    for (auto chunk : chunks) {
        if (!chunk.empty())
            return chunk;
    }
    return {};
}

inline std::span<const std::byte> first_nonempty(
    std::span<const std::span<const std::byte>> chunks,
    std::size_t splat) noexcept
{
    if (chunks.empty())
        return {};
    for (auto chunk : chunks.first(chunks.size() - 1)) {
        if (!chunk.empty())
            return chunk;
    }
    if (splat != 0 && !chunks.back().empty())
        return chunks.back();
    return {};
}

inline std::span<std::byte> first_nonempty(
    std::span<std::span<std::byte>> chunks) noexcept
{
    for (auto chunk : chunks) {
        if (!chunk.empty())
            return chunk;
    }
    return {};
}

} // namespace detail

/// Buffered asynchronous writer base, modeled on Zig's `std.Io.Writer`.
///
/// Zig's writer design is excellent: the concrete writer object owns the buffer
/// cursors, while a tiny cold-path vtable handles the rare operation that cannot
/// be satisfied from the buffer. `byte_writer` maps that idea into C++ by making
/// the hot path ordinary non-virtual member functions that return `hope<void>`;
/// if bytes fit in the buffer, awaiting them does not suspend or touch the deck.
///
/// Pending output is `buffer_[seek_..end_]`; bytes before `seek_` have already
/// been consumed by the sink. Unlike Zig's current writer, this base keeps a
/// reader-like seek cursor so partially drained buffered output is represented
/// directly instead of being hidden during rebase/drain logic.
///
/// An empty writer buffer is valid. It means "always cold path": non-empty
/// writes call `drain_more()` immediately.
class byte_writer
{
public:
    explicit byte_writer(std::span<std::byte> buffer)
        : buffer_(buffer)
    {}

    explicit byte_writer(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_)
    {}

    byte_writer(const byte_writer &) = delete;
    byte_writer & operator=(const byte_writer &) = delete;

    byte_writer(byte_writer &&) = delete;
    byte_writer & operator=(byte_writer &&) = delete;

    virtual ~byte_writer() = default;

    /// Return the bytes currently staged for the sink.
    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.subspan(seek_, end_ - seek_);
    }

    /// Return the number of bytes currently staged for the sink.
    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_ - seek_;
    }

    /// Return writable buffer capacity after the currently staged bytes.
    ///
    /// Source adapters may fill this span directly and then call `advance()`.
    /// The span is invalidated by any later writer operation.
    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    /// Commit `n` bytes that were written directly into `unused_capacity()`.
    void advance(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw buffer_error{"writer advanced past buffer capacity"};
        end_ += n;
    }

    /// Ensure `capacity` bytes can be appended while preserving recent output.
    ///
    /// This is the C++ mapping of Zig writer `rebase(w, preserve, capacity)`.
    /// If there is already enough unused capacity, it returns ready. Otherwise
    /// it may drain buffered output, but the most recent `preserve` bytes of the
    /// currently buffered output remain staged. Because `byte_writer` has both
    /// `seek_` and `end_`, this can honestly drain only the non-preserved prefix
    /// instead of temporarily hiding the preserved suffix as Zig currently does.
    hope<void> rebase(std::size_t preserve, std::size_t capacity)
    {
        require_preserved_capacity(preserve, capacity);
        if (unused_capacity().size() >= capacity)
            return hope<void>::ready();
        if (can_compact_for(capacity)) {
            compact_buffered();
            return hope<void>::ready();
        }
        return rebase_slow(preserve, capacity);
    }

    /// Return writable capacity after preserving recent output if needed.
    ///
    /// The returned span is not advanced. It is invalidated by any later writer
    /// operation. If `preserve` is zero this is equivalent to
    /// `writable_slice_greedy(minimum)`.
    hope<std::span<std::byte>>
    writable_slice_greedy_preserve(std::size_t preserve, std::size_t minimum)
    {
        auto ready = rebase(preserve, minimum);
        if (ready.is_ready())
            return hope<std::span<std::byte>>::ready(unused_capacity());
        return writable_slice_greedy_preserve_slow(std::move(ready));
    }

    /// Return writable capacity, draining or compacting if needed.
    hope<std::span<std::byte>>
    writable_slice_greedy(std::size_t minimum)
    {
        return writable_slice_greedy_preserve(0, minimum);
    }

    /// Reserve exactly `len` bytes while preserving recent output if needed.
    ///
    /// The returned span has length `len` and has already been committed with
    /// `advance(len)`, matching Zig's `writableSlicePreserve`. The caller should
    /// write to the returned span before any other writer operation observes the
    /// newly committed bytes.
    hope<std::span<std::byte>>
    writable_slice_preserve(std::size_t preserve, std::size_t len)
    {
        auto slice = writable_slice_greedy_preserve(preserve, len);
        if (slice.is_ready()) {
            auto out = slice.take_ready().first(len);
            advance(len);
            return hope<std::span<std::byte>>::ready(out);
        }
        return writable_slice_preserve_slow(std::move(slice), len);
    }

    /// Reserve exactly `len` bytes.
    hope<std::span<std::byte>> writable_slice(std::size_t len)
    {
        return writable_slice_preserve(0, len);
    }

    /// Drain all currently buffered output to the concrete sink.
    ///
    /// Returns ready when there is no buffered data. Otherwise this repeatedly
    /// calls `drain_more()` until `buffered_size()` reaches zero.
    hope<void> flush()
    {
        if (buffered_size() == 0) {
            reset_if_empty();
            return hope<void>::ready();
        }
        return flush_slow();
    }

    /// Write bytes, buffering them when capacity is available.
    ///
    /// This is a one-call convenience: when `bytes` does not fit in the current
    /// buffer it may drain pending output and then either write directly to the
    /// sink or stage a suffix in the buffer.
    hope<void> write(std::span<const std::byte> bytes)
    {
        if (bytes.empty())
            return hope<void>::ready();

        if (bytes.size() <= unused_capacity().size()) {
            append_to_buffer(bytes);
            return hope<void>::ready();
        }

        return write_slow(bytes);
    }

    /// Write `pattern` repeated `splat` times.
    ///
    /// Mirrors Zig's splat semantics: the final chunk passed to `drain_more()`
    /// represents a span that should be repeated, avoiding allocation of a range
    /// containing the same span many times.
    hope<void> write_splat(
        std::span<const std::byte> pattern,
        std::size_t splat)
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

    /// Write a UTF-8 text pattern repeatedly as bytes.
    hope<void> write_splat(std::string_view pattern, std::size_t splat)
    {
        return write_splat(as_bytes(pattern), splat);
    }

    /// Write an owned string, keeping its storage alive across suspension.
    task<void> write(std::string text)
    {
        co_await write(as_bytes(std::string_view{text}));
    }

    /// Write a borrowed UTF-8 string view as bytes.
    ///
    /// If this returns a non-ready `hope`, the caller must ensure `text` remains
    /// alive until the await completes.
    hope<void> write(std::string_view text)
    {
        return write(as_bytes(text));
    }

    /// Format text with `std::format` and write the resulting bytes.
    ///
    /// This is the C++ counterpart to Zig writer `.print(fmt, ...)`. Formatting
    /// materializes a temporary `std::string`; the resulting write still uses the
    /// normal buffered writer semantics.
    template<typename... Args>
    task<void> print(std::format_string<Args...> fmt, Args &&... args)
    {
        co_await write(std::format(fmt, std::forward<Args>(args)...));
    }

    /// Write each chunk in a byte/text chunk range.
    ///
    /// Chunks may be byte spans or UTF-8 string-like views. The range itself must
    /// remain valid until the returned task completes.
    template<detail::byte_writer_chunk_range Chunks>
    task<void> write(Chunks && chunks)
    {
        for (auto && chunk : chunks)
            co_await write(std::forward<decltype(chunk)>(chunk));
    }

    /// Write bytes or byte/text chunks, then flush the writer.
    template<typename Chunks>
        requires detail::byte_writer_chunk<Chunks>
            || detail::byte_writer_chunk_range<Chunks>
    task<void> write_all(Chunks && chunks)
    {
        co_await write(std::forward<Chunks>(chunks));
        co_await flush();
    }

    /// Format text, write it, then flush the writer.
    template<typename... Args>
    task<void> print_all(std::format_string<Args...> fmt, Args &&... args)
    {
        co_await print(fmt, std::forward<Args>(args)...);
        co_await flush();
    }

protected:
    /// Cold-path sink operation.
    ///
    /// Implementations transfer bytes from `chunks` to the concrete backend and
    /// return how many bytes were accepted. The return value may describe a
    /// partial transfer, but it must be no larger than the total logical byte
    /// count. Returning zero when any bytes are available is a protocol error for
    /// the base writer, which relies on progress to avoid an infinite flush loop.
    ///
    /// The last span in `chunks` is repeated `splat` times. Earlier spans are
    /// sent once. This follows Zig's `Writer.drain(data, splat)` trick and is
    /// what lets `write_splat()` express repeated output without allocating or
    /// manufacturing a large vector of identical spans.
    virtual hope<std::size_t> drain_more(
        std::span<const std::span<const std::byte>> chunks,
        std::size_t splat) = 0;

private:
    void append_to_buffer(std::span<const std::byte> bytes)
    {
        std::memcpy(buffer_.data() + end_, bytes.data(), bytes.size());
        advance(bytes.size());
    }

    void append_splat_to_buffer(
        std::span<const std::byte> pattern,
        std::size_t splat)
    {
        for (auto i = std::size_t{0}; i < splat; ++i)
            append_to_buffer(pattern);
    }

    void reset_if_empty() noexcept
    {
        if (seek_ == end_)
            seek_ = end_ = 0;
    }

    void consume_buffered(std::size_t n)
    {
        if (n > buffered_size())
            throw buffer_error{"writer overreported drained bytes"};
        seek_ += n;
        reset_if_empty();
    }

    static void require_progress(
        std::size_t written,
        std::size_t available)
    {
        if (written == 0)
            throw buffer_error{"writer made no progress"};
        if (written > available)
            throw buffer_error{"writer overreported written bytes"};
    }

    static std::size_t repeated_size(std::size_t size, std::size_t count)
    {
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
            throw buffer_error{"byte count overflow"};
        return size * count;
    }

    static void require_preserved_capacity(
        std::size_t preserve,
        std::size_t capacity,
        std::size_t total)
    {
        if (preserve > total || capacity > total - preserve)
            throw buffer_error{"writer buffer is too small"};
    }

    void require_preserved_capacity(
        std::size_t preserve,
        std::size_t capacity) const
    {
        require_preserved_capacity(preserve, capacity, buffer_.size());
    }

    bool can_compact_for(std::size_t capacity) const noexcept
    {
        return buffer_.size() - buffered_size() >= capacity;
    }

    void compact_buffered()
    {
        auto pending = buffered_size();
        if (seek_ != 0 && pending != 0)
            std::memmove(buffer_.data(), buffer_.data() + seek_, pending);
        seek_ = 0;
        end_ = pending;
    }

    task<void> flush_slow()
    {
        while (buffered_size() != 0) {
            auto pending = buffered();
            auto chunks = std::array{pending};
            auto written = co_await drain_more(std::span{chunks}, 1);
            require_progress(written, pending.size());
            consume_buffered(written);
        }
    }

    task<void> drain_pending_and_direct(std::span<const std::byte> bytes)
    {
        auto remaining = bytes;
        while (buffered_size() != 0 || !remaining.empty()) {
            auto pending = buffered();
            auto chunks = pending.empty()
                ? std::array{remaining, std::span<const std::byte>{}}
                : std::array{pending, remaining};
            auto count = pending.size() + remaining.size();
            auto written = co_await drain_more(std::span{chunks}, 1);
            require_progress(written, count);

            auto from_pending = std::min(written, pending.size());
            consume_buffered(from_pending);
            written -= from_pending;
            if (written > remaining.size())
                throw buffer_error{"writer overreported written bytes"};
            remaining = remaining.subspan(written);
        }
    }

    task<void> drain_pending_and_splat(
        std::span<const std::byte> pattern,
        std::size_t splat)
    {
        auto partial = std::span<const std::byte>{};
        while (buffered_size() != 0 || !partial.empty() || splat != 0) {
            auto pending = buffered();
            auto chunks = std::array{
                std::span<const std::byte>{},
                std::span<const std::byte>{},
                std::span<const std::byte>{},
            };
            auto chunk_count = std::size_t{0};
            if (!pending.empty())
                chunks[chunk_count++] = pending;
            if (!partial.empty())
                chunks[chunk_count++] = partial;
            if (splat != 0)
                chunks[chunk_count++] = pattern;

            auto available = pending.size()
                + partial.size()
                + repeated_size(pattern.size(), splat);
            auto repeated = splat == 0 ? std::size_t{1} : splat;
            auto written = co_await drain_more(
                std::span{chunks}.first(chunk_count),
                repeated);
            require_progress(written, available);

            auto from_pending = std::min(written, pending.size());
            consume_buffered(from_pending);
            written -= from_pending;

            auto from_partial = std::min(written, partial.size());
            partial = partial.subspan(from_partial);
            written -= from_partial;

            if (written == 0)
                continue;

            auto full = std::min(splat, written / pattern.size());
            splat -= full;
            written -= full * pattern.size();
            if (written != 0) {
                if (written > pattern.size() || splat == 0)
                    throw buffer_error{"writer overreported written bytes"};
                partial = pattern.subspan(written);
                --splat;
            }
        }
    }

    task<void> write_slow(std::span<const std::byte> bytes)
    {
        auto remaining = bytes;
        while (!remaining.empty()) {
            if (remaining.size() >= buffer_.size()) {
                co_await drain_pending_and_direct(remaining);
                co_return;
            }

            if (unused_capacity().empty())
                co_await flush();

            auto n = std::min(unused_capacity().size(), remaining.size());
            std::memcpy(buffer_.data() + end_, remaining.data(), n);
            advance(n);
            remaining = remaining.subspan(n);
        }
    }

    task<void> rebase_slow(std::size_t preserve, std::size_t capacity)
    {
        while (unused_capacity().size() < capacity) {
            if (can_compact_for(capacity)) {
                compact_buffered();
                co_return;
            }

            auto preserved = std::min(preserve, buffered_size());
            auto drainable = buffered_size() - preserved;
            if (drainable == 0)
                throw buffer_error{"writer buffer is too small"};

            auto prefix = buffered().first(drainable);
            auto chunks = std::array{prefix};
            auto written = co_await drain_more(std::span{chunks}, 1);
            require_progress(written, prefix.size());
            consume_buffered(written);
        }
    }

    task<std::span<std::byte>>
    writable_slice_greedy_preserve_slow(hope<void> ready)
    {
        co_await std::move(ready);
        co_return unused_capacity();
    }

    task<std::span<std::byte>>
    writable_slice_preserve_slow(
        hope<std::span<std::byte>> slice,
        std::size_t len)
    {
        auto out = (co_await std::move(slice)).first(len);
        advance(len);
        co_return out;
    }

    task<void> write_splat_slow(
        std::span<const std::byte> pattern,
        std::size_t splat)
    {
        if (pattern.size() >= buffer_.size()) {
            co_await drain_pending_and_splat(pattern, splat);
            co_return;
        }

        while (splat != 0) {
            if (unused_capacity().size() < pattern.size())
                co_await flush();

            auto fit = std::min(
                splat,
                unused_capacity().size() / pattern.size());
            append_splat_to_buffer(pattern, fit);
            splat -= fit;
        }
    }

    std::vector<std::byte> owned_buffer_;
    std::span<std::byte> buffer_;
    std::size_t seek_ = 0;
    std::size_t end_ = 0;
};

/// Writer that stores bytes in a fixed caller-owned span.
class fixed_byte_writer final : public byte_writer
{
public:
    explicit fixed_byte_writer(std::span<std::byte> buffer)
        : byte_writer(buffer)
    {}

private:
    hope<std::size_t>
    drain_more(
        std::span<const std::span<const std::byte>>,
        std::size_t) override
    {
        throw buffer_error{"fixed writer is full"};
    }
};

/// Writer that accepts and ignores all bytes.
class discarding_byte_writer final : public byte_writer
{
public:
    explicit discarding_byte_writer(std::span<std::byte> buffer = {})
        : byte_writer(buffer)
    {}

private:
    hope<std::size_t>
    drain_more(
        std::span<const std::span<const std::byte>> chunks,
        std::size_t splat) override
    {
        return hope<std::size_t>::ready(detail::byte_size(chunks, splat));
    }
};

/// Writer for a file descriptor.
class fd_sink final : public byte_writer
{
public:
    explicit fd_sink(int fd, std::span<std::byte> buffer)
        : byte_writer(buffer)
        , fd_(fd)
    {}

    explicit fd_sink(int fd, std::size_t buffer_size = 4096)
        : byte_writer(buffer_size)
        , fd_(fd)
    {}

    /// Bytes successfully written to the file descriptor by completed drains.
    ///
    /// Bytes still staged in the writer buffer are not included until `flush()`
    /// or another drain sends them to the descriptor.
    [[nodiscard]] std::size_t written_size() const noexcept
    {
        return written_;
    }

private:
    hope<std::size_t>
    drain_more(
        std::span<const std::span<const std::byte>> chunks,
        std::size_t splat) override
    {
        return drain_more_task(chunks, splat);
    }

    task<std::size_t>
    drain_more_task(
        std::span<const std::span<const std::byte>> chunks,
        std::size_t splat)
    {
        auto src = detail::first_nonempty(chunks, splat);
        while (true) {
            try {
                auto n = co_await op::write_some{
                    .fd = fd_,
                    .buffer = src,
                    .offset = -1,
                };
                written_ += n;
                co_return n;
            } catch (const interrupted_system_call &) {
            }
        }
    }

    int fd_ = -1;
    std::size_t written_ = 0;
};

inline fd_sink standard_output(std::size_t buffer_size = 4096)
{
    return fd_sink{STDOUT_FILENO, buffer_size};
}

inline fd_sink standard_output_writer(std::size_t buffer_size = 4096)
{
    return standard_output(buffer_size);
}

/// Writer for a connected socket.
class socket_sink final : public byte_writer
{
public:
    explicit socket_sink(
        int fd,
        std::span<std::byte> buffer,
        int flags = 0)
        : byte_writer(buffer)
        , fd_(fd)
        , flags_(flags)
    {}

    explicit socket_sink(
        int fd,
        int flags = 0,
        std::size_t buffer_size = 4096)
        : byte_writer(buffer_size)
        , fd_(fd)
        , flags_(flags)
    {}

private:
    hope<std::size_t>
    drain_more(
        std::span<const std::span<const std::byte>> chunks,
        std::size_t splat) override
    {
        return drain_more_task(chunks, splat);
    }

    task<std::size_t>
    drain_more_task(
        std::span<const std::span<const std::byte>> chunks,
        std::size_t splat)
    {
        auto src = detail::first_nonempty(chunks, splat);
        while (true) {
            try {
                co_return co_await op::send_some{
                    .fd = fd_,
                    .buffer = src,
                    .flags = flags_,
                };
            } catch (const interrupted_system_call &) {
            }
        }
    }

    int fd_ = -1;
    int flags_ = 0;
};

class metered_byte_sink final : public byte_writer
{
public:
    metered_byte_sink(
        byte_writer & inner,
        std::span<std::byte> buffer,
        std::function<void(std::size_t)> progress)
        : byte_writer(buffer)
        , inner_(inner)
        , progress_(std::move(progress))
    {}

    metered_byte_sink(
        byte_writer & inner,
        std::function<void(std::size_t)> progress,
        std::size_t buffer_size = 4096)
        : byte_writer(buffer_size)
        , inner_(inner)
        , progress_(std::move(progress))
    {}

private:
    hope<std::size_t>
    drain_more(
        std::span<const std::span<const std::byte>> chunks,
        std::size_t splat) override
    {
        return drain_more_task(chunks, splat);
    }

    task<std::size_t>
    drain_more_task(
        std::span<const std::span<const std::byte>> chunks,
        std::size_t splat)
    {
        if (chunks.empty())
            co_return std::size_t{0};
        auto total = detail::byte_size(chunks, splat);
        for (auto chunk : chunks.first(chunks.size() - 1))
            co_await inner_.write(chunk);
        for (auto i = std::size_t{0}; i < splat; ++i)
            co_await inner_.write(chunks.back());
        co_await inner_.flush();
        if (total != 0)
            progress_(total);
        co_return total;
    }

    byte_writer & inner_;
    std::function<void(std::size_t)> progress_;
};

inline metered_byte_sink meter_sink(
    byte_writer & inner,
    std::function<void(std::size_t)> progress,
    std::size_t buffer_size = 4096)
{
    return metered_byte_sink{inner, std::move(progress), buffer_size};
}

template<typename Chunks>
    requires detail::byte_writer_chunk<Chunks>
        || detail::byte_writer_chunk_range<Chunks>
inline task<void> write_all(byte_writer & writer, Chunks && chunks)
{
    co_await writer.write_all(std::forward<Chunks>(chunks));
}

// ===========================================================================
// Design note: the Zig std.Io paradigm and how nxt adapts it
// ===========================================================================
//
// This reader/writer family is a deliberate port of Zig's post-0.15
// `std.Io.Reader`/`std.Io.Writer` design, adapted to C++ stackless coroutines.
// The Zig API looks idiosyncratic at first; the shape is load-bearing, so it
// is worth recording why, and which parts we adopted, diverged from, or still
// owe.
//
// --- What Zig actually does -------------------------------------------------
//
// `std.Io.Reader` is a *struct that contains the buffer*:
//
//     vtable: *const VTable,  buffer: []u8,  seek: usize,  end: usize
//
// "Buffered reader" is therefore not a wrapper you compose around a reader --
// the reader *is* the buffer. The vtable is small and is the COLD path:
//
//     stream(r, w, limit)   -- mandatory; push bytes into a Writer
//     discard(r, limit)     -- default; skip bytes without exposing them
//     readVec(r, [][]u8)    -- default; vectored pull into caller slices
//     rebase(r, capacity)   -- default; make room (memmove) for `capacity`
//
// The "obvious" primitive is `stream` (push into a Writer), not a pull, because
// that is what lets a file reader splice straight into a socket writer via
// `sendFile`/`copy_file_range` without the bytes ever touching `buffer`. The
// pull APIs are *derived*: `readVec` defaults to calling `stream`, and "fill my
// own buffer" is just `readVec` with a single zero-length destination slice --
// the convention "data[0].len == 0 => write into Reader.buffer".
//
// The hot path (`take`, `peek`, `takeInt`, ...) is concrete and inline; it only
// calls the vtable when the buffer runs dry. Zig splits `fill` from
// `fillUnbuffered` *specifically* so the "already buffered?" check inlines with
// a branch hint; their own comment notes that merging them regressed hot
// parsers by 5x, because callers paid a real function call just to discover the
// byte was already in the buffer. The whole design is "make the buffered case
// free; pay the vtable only on a refill."
//
// Crucially, in Zig this is all SYNCHRONOUS. There is no function coloring,
// because Zig's concurrency is *stackful* (fibers in the `Io` implementation):
// a blocking op swaps the whole stack out at the bottom, invisibly, so the
// Reader and the parser above it are ordinary synchronous code.
//
// --- Where C++ forces us to diverge ----------------------------------------
//
// We are *stackless*. A `co_await` can only suspend the frame it is written in,
// and that property is viral, so an async parser must be a coroutine and reads
// must be awaited -- we cannot make suspension invisible the way fibers do.
//
// The mandatory source cold path is now `stream_more()`, the Zig-shaped verb:
// push up to `limit` bytes into a `byte_writer`. Like Zig, an implementation may
// also choose to put bytes in its own reader buffer and return zero streamed
// bytes; the next public `stream()`/`take()` call will consume those buffered
// bytes through the hot path. Pull-shaped refill is derived by pointing a fixed
// writer at the reader's unused capacity, so source bytes still land in
// `buffer_` before borrowed-span APIs (`peek`/`take`) expose them. This gives us
// the structure of Zig's "stream into a writer, and refill is just streaming into
// my own buffer" without yet caring about fd-to-fd sendfile-style optimization.
//
// The remaining divergence is that nxt readers still require at least one byte
// of storage. Zig can express "fill my Reader.buffer" through `readVec`'s
// special empty-slice convention; our concrete refill has nowhere to put source
// bytes unless the reader owns or borrows a real span, even if it is only one
// byte. A zero-buffer reader can make sense once the cold surface is rich
// enough to avoid pull-shaped refill entirely, but today's `peek`/`take` APIs
// need a place to park bytes.
//
// The hot/cold split is mirrored by `hope<T>` (see task.hpp). The buffered case
// returns `hope<...>::ready(span)` -- a synchronous value, no coroutine frame,
// no deck round-trip -- and only a miss builds a `*_slow` task that is spliced
// as the awaiter's continuation. This is the C++ stackless answer to Zig's
// `fill`/`fillUnbuffered` inlining trick: without it, `co_await reader.take(n)`
// on already-buffered data would still bounce the deck (lazy `task<T>` is never
// `await_ready`), which is exactly the per-field trampoline this design exists
// to kill.
//
// `stream_more()` itself returns `hope<read_result>`, not `task<read_result>`.
// That is the payoff lever: a layer that already holds bytes (decrypted TLS
// plaintext, an in-memory span) streams or refills SYNCHRONOUSLY, so a
// fully-buffered read composes with zero suspensions through a whole stack of
// readers (socket -> tls -> http_body -> sse). It is the "eager wish" idea
// applied to the cold verb, achieved without any wand change.
//
// The wider Zig vocabulary is now structural: `stream()` and `read_vec()` use
// buffered bytes when they have them and otherwise call the corresponding cold
// virtual (`stream_more()` / `read_vec_more()`). `discard()` uses buffered bytes
// when it has them and otherwise calls overridable `discard_more()`. Refill is
// just `read_vec_more({unused_capacity()})`, i.e. readVec into this reader's
// own buffer.
//
// On the writer side we intentionally one-up Zig a little: `byte_writer` has a
// reader-like `seek_` as well as `end_`, so partially drained buffered output
// is represented honestly as `buffer_[seek_..end_]`. That makes Zig-style
// `rebase(preserve, capacity)` direct: drain only the non-preserved prefix,
// keep the recent suffix staged, and compact when contiguous capacity is needed.
// Zig's writer source has a TODO wishing for this because its default rebase
// logic temporarily hides preserved bytes by mutating `end`.
//
// --- What we still owe to fully adopt the paradigm --------------------------
//
// TODO(zig-stream): teach concrete fd/socket readers and writers about each
//   other so `stream_more()` can eventually use sendfile/readv/writev-shaped
//   paths. Today it has the right API shape but still moves ordinary spans.
// TODO(zig-readvec): teach fd/socket/task-backed sources real scatter reads.
//   The virtual slot exists, but the generic default still streams into only
//   the first non-empty destination, matching Zig's simple default.
// TODO(zig-discard): optimized `discard_more(limit)` overrides so protocols can
//   skip bytes (chunked trailers, body skip-to-end) without buffering and
//   copying them through a writer-shaped shim.
// TODO(zig-rebase): use the virtual `rebase_more` slot for a ring- or
//   mmap-backed reader that can make room differently from memmove.
// TODO(eager-wand): push the synchronous-completion idea of `stream_more()` down
//   to the wish layer -- an honest `waiter::await_ready()` plus a sync path in
//   `wand::prepare` -- so a warm `read_some` on the fd also skips the
//   round-trip. At that point the buffered reader can BE a wand and `hope`
//   dissolves into a single "maybe already here, else suspends" awaitable
//   shared by wishes and readers alike. That is the endgame this whole family
//   is shaped toward.

/// Buffered asynchronous reader base, modeled on Zig's `std.Io.Reader`.
///
/// Zig's post-0.15 reader design deserves real praise: the reader itself owns
/// the buffer and exposes a small cold-path vtable, so parsers pay almost
/// nothing when the requested bytes are already buffered. `byte_reader` ports
/// that shape to C++ stackless coroutines by making the hot-path operations
/// (`peek()`, `take()`, `take_struct()`, and friends) concrete, non-virtual, and
/// `hope`-returning. A buffered hit is ready immediately; only a buffer miss
/// builds or awaits a coroutine.
///
/// The mandatory cold primitive is `stream_more()`, following Zig's `stream`:
/// push bytes into a `byte_writer`. Pull APIs such as `read_vec()` and refill
/// are derived from that operation by default. Implementations may also put
/// bytes into their own reader buffer and return zero streamed bytes; the next
/// public operation will consume those bytes through the ordinary hot path.
///
/// Unlike Zig, this C++ reader currently requires at least one byte of buffer
/// storage. Borrowed-span APIs need somewhere stable to park source bytes across
/// awaits, and we do not yet have Zig's exact empty-slice `readVec` convention.
class byte_reader
{
public:
    /// Construct a reader with owned buffer storage.
    explicit byte_reader(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_)
    {
        if (owned_buffer_.empty())
            throw buffer_error{"reader buffer is empty"};
    }

    /// Construct a reader over caller-owned buffer storage.
    ///
    /// The storage must outlive the reader.
    explicit byte_reader(std::span<std::byte> buffer)
        : buffer_(buffer)
    {
        if (buffer.empty())
            throw buffer_error{"reader buffer is empty"};
    }

    virtual ~byte_reader() = default;

    byte_reader(const byte_reader &) = delete;
    byte_reader & operator=(const byte_reader &) = delete;

    byte_reader(byte_reader &&) = delete;
    byte_reader & operator=(byte_reader &&) = delete;

    /// Return currently buffered bytes.
    ///
    /// The returned span is invalidated by any operation that refills, rebases,
    /// or consumes the reader.
    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.subspan(seek_, end_ - seek_);
    }

    /// Return the number of currently buffered bytes.
    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_ - seek_;
    }

    /// Return writable storage after the currently buffered bytes.
    ///
    /// Derived readers may write source bytes here and call protected
    /// `advance()`.
    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    /// Ensure that `capacity` bytes can fit from the current seek position.
    ///
    /// The default cold path memmoves buffered data to the start of the buffer.
    /// Derived readers may override `rebase_more()` for ring buffers or other
    /// storage strategies.
    void rebase(std::size_t capacity)
    {
        if (capacity > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffer_.size() - seek_ >= capacity)
            return;
        rebase_more(capacity);
        if (buffer_.size() - seek_ < capacity)
            throw buffer_error{"reader rebase did not make enough room"};
    }

    /// Ensure at least `n` bytes are buffered.
    ///
    /// If the bytes are already present this returns ready and does not suspend.
    /// If EOF occurs before `n` bytes are available, throws `end_of_stream`.
    hope<void> fill(std::size_t n)
    {
        if (n > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n)
            return hope<void>::ready();

        return fill_slow(n);
    }

    /// Refill by one source operation when the buffer is not full.
    ///
    /// The returned byte count is the number of bytes appended to this reader's
    /// buffer. A zero byte count is not necessarily EOF unless `read_result::eof`
    /// is also true.
    hope<read_result> fill_more()
    {
        if (buffered_size() == buffer_.size())
            throw buffer_error{"reader buffer is full"};
        rebase(buffered_size() + 1);
        return fill_more_without_rebase();
    }

    /// Borrow the next `n` bytes without consuming them.
    ///
    /// The returned span remains valid only until the next operation that may
    /// mutate the reader buffer.
    hope<std::span<const std::byte>> peek(std::size_t n)
    {
        if (n > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n)
            return hope<std::span<const std::byte>>::ready(
                buffered().first(n));
        return peek_slow(n);
    }

    /// Copy a trivially copyable value from the next bytes without consuming it.
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    hope<T> peek_struct()
    {
        auto bytes = peek(sizeof(T));
        if (bytes.is_ready())
            return hope<T>::ready(copy_struct<T>(bytes.take_ready()));
        return peek_struct_slow<T>(std::move(bytes));
    }

    /// Consume and return the next borrowed chunk, up to `limit` bytes.
    ///
    /// The returned span may be empty. `std::nullopt` is the EOF signal.
    hope<std::optional<std::span<const std::byte>>>
    take_some(std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (buffered_size() == 0) {
            auto read = fill_more();
            if (!read.is_ready())
                return take_some_slow(std::move(read), limit);
            auto result = read.take_ready();
            if (result.eof && result.bytes == 0)
                return hope<std::optional<std::span<const std::byte>>>::
                    ready(std::nullopt);
            if (result.bytes == 0)
                return hope<std::optional<std::span<const std::byte>>>::
                    ready(buffered().first(0));
        }

        auto n = std::min(limit, buffered_size());
        auto out = buffered().first(n);
        toss(n);
        return hope<std::optional<std::span<const std::byte>>>::ready(out);
    }

    /// Discard `n` already-buffered bytes.
    void toss(std::size_t n)
    {
        if (n > buffered_size())
            throw buffer_error{"reader consumed past buffered input"};
        seek_ += n;
    }

    /// Consume and return exactly `n` borrowed bytes.
    ///
    /// Throws `buffer_error` if `n` is larger than this reader's whole buffer,
    /// and `end_of_stream` if EOF arrives before `n` bytes are available.
    hope<std::span<const std::byte>> take(std::size_t n)
    {
        if (n > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n) {
            auto out = buffered().first(n);
            toss(n);
            return hope<std::span<const std::byte>>::ready(out);
        }
        return take_slow(n);
    }

    /// Consume exactly `n` bytes and view them as UTF-8 text.
    hope<std::string_view> take_string_view(std::size_t n)
    {
        auto bytes = take(n);
        if (bytes.is_ready())
            return hope<std::string_view>::ready(
                as_string_view(bytes.take_ready()));
        return take_string_view_slow(std::move(bytes));
    }

    /// Consume and copy a trivially copyable value.
    ///
    /// Returns `std::nullopt` only when EOF is reached before any bytes of the
    /// value are available. EOF in the middle of the value is `end_of_stream`.
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    hope<std::optional<T>> take_struct()
    {
        if (buffered_size() >= sizeof(T)) {
            auto value = copy_struct<T>(buffered());
            toss(sizeof(T));
            return hope<std::optional<T>>::ready(value);
        }

        return take_struct_slow<T>();
    }

    /// Consume and return bytes up to `delimiter`, excluding the delimiter.
    ///
    /// The delimiter is consumed. Throws `end_of_stream` if EOF arrives before
    /// the delimiter, and `buffer_error` if the buffer fills before the delimiter
    /// can be found.
    hope<std::span<const std::byte>>
    take_until(std::span<const std::byte> delimiter)
    {
        if (delimiter.empty())
            throw buffer_error{"empty delimiter"};

        auto available = buffered();
        auto cut = find_bytes(available, delimiter);
        if (cut < available.size()) {
            auto out = available.first(cut);
            seek_ += cut + delimiter.size();
            return hope<std::span<const std::byte>>::ready(out);
        }

        return take_until_slow(delimiter);
    }

    /// Consume and return bytes up to a UTF-8 text delimiter.
    hope<std::span<const std::byte>>
    take_until(std::string_view delimiter)
    {
        return take_until(as_bytes(delimiter));
    }

    /// Pull one available chunk into caller-owned destination spans.
    ///
    /// If buffered bytes exist, they are copied synchronously into `dsts`.
    /// Otherwise this calls `read_vec_more()`. The operation is intentionally
    /// chunk-shaped, not "read exactly all destinations".
    hope<read_result> read_vec(std::span<std::span<std::byte>> dsts)
    {
        if (buffered_size() != 0) {
            auto n = copy_buffered_to(dsts);
            return hope<read_result>::ready(read_result{.bytes = n});
        }

        auto first = first_nonempty_index(dsts);
        if (!first)
            return hope<read_result>::ready(read_result{});
        return read_vec_more(dsts.subspan(*first));
    }

    /// Pull one available chunk into a single caller-owned destination span.
    hope<read_result> read(std::span<std::byte> dst)
    {
        auto dsts = std::array{dst};
        return read_vec(std::span{dsts});
    }

    /// Transfer one available chunk to `writer`, up to `limit` bytes.
    ///
    /// This is the central Zig-shaped operation. It may return zero bytes without
    /// EOF if the implementation filled its own reader buffer instead of the
    /// writer; callers that want all bytes should loop until EOF or their limit
    /// is satisfied.
    hope<read_result> stream(
        byte_writer & writer,
        std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (limit == 0)
            return hope<read_result>::ready(read_result{});

        if (buffered_size() == 0)
            return stream_more(writer, limit);

        return stream_buffered(writer, limit);
    }

    /// Discard one available chunk, up to `limit` bytes.
    ///
    /// Buffered bytes are tossed synchronously. Otherwise this delegates to
    /// `discard_more()`, whose default implementation streams into a discarding
    /// writer.
    hope<read_result> discard(
        std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (limit == 0)
            return hope<read_result>::ready(read_result{});

        if (buffered_size() == 0)
            return discard_more(limit);

        auto n = std::min(limit, buffered_size());
        toss(n);
        return hope<read_result>::ready(read_result{.bytes = n});
    }

protected:
    /// Return the full mutable reader storage.
    ///
    /// This is for derived storage strategies and adapters; ordinary refill code
    /// should usually prefer `unused_capacity()`.
    [[nodiscard]] std::span<std::byte> buffer_storage() noexcept
    {
        return buffer_;
    }

    /// Commit `n` bytes that a derived reader wrote into `unused_capacity()`.
    void advance(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw buffer_error{"reader advanced past buffer capacity"};
        end_ += n;
    }

    /// Mandatory cold-path source operation.
    ///
    /// Implementations should transfer up to `limit` bytes from the underlying
    /// source into `writer`, returning the number of bytes logically advanced.
    /// The number may be smaller than `limit`; a zero byte count does not by
    /// itself mean EOF.
    ///
    /// To preserve Zig's reader shape, an implementation may instead append bytes
    /// to this reader's own buffer using `unused_capacity()` and `advance()`, then
    /// return `{.bytes = 0, .eof = false}`. This is the preferred fallback when
    /// the destination writer has no writable capacity; do not allocate hidden
    /// temporary storage just to satisfy `stream_more()`.
    ///
    /// Set `read_result::eof` only when the source has reached EOF. EOF with
    /// bytes is allowed; EOF with zero bytes is the usual terminal signal.
    virtual hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) = 0;

    /// Cold-path vectored pull operation.
    ///
    /// Called only when the public `read_vec()` has no buffered bytes to copy and
    /// at least one destination span is non-empty. The default mirrors Zig's
    /// simple default: stream into a fixed writer over the first non-empty
    /// destination. Override this for real scatter reads.
    virtual hope<read_result> read_vec_more(
        std::span<std::span<std::byte>> dsts)
    {
        auto dst = detail::first_nonempty(dsts);
        if (dst.empty())
            return hope<read_result>::ready(read_result{});

        auto writer = fixed_byte_writer{dst};
        auto result = stream_more(writer, dst.size());
        if (result.is_ready())
            return hope<read_result>::ready(
                finish_fixed_write(writer, result.take_ready()));
        return read_vec_more_slow(dst);
    }

    /// Cold-path discard operation.
    ///
    /// Called only when the public `discard()` has no buffered bytes to toss. The
    /// default streams into a discarding writer backed by this reader's buffer.
    /// Override this when the backend can skip bytes more directly.
    virtual hope<read_result> discard_more(std::size_t limit)
    {
        auto writer = discarding_byte_writer{buffer_};
        auto result = stream_more(writer, limit);
        if (result.is_ready())
            return hope<read_result>::ready(result.take_ready());
        return discard_more_slow(limit);
    }

    /// Cold-path rebase operation.
    ///
    /// Must make it possible for `capacity` bytes to fit from `seek_` onward, or
    /// leave the object in a state where `rebase()` can report `buffer_error`.
    /// The default memmoves buffered bytes to the start of `buffer_`.
    virtual void rebase_more(std::size_t)
    {
        auto pending = buffered_size();
        std::memmove(buffer_.data(), buffer_.data() + seek_, pending);
        seek_ = 0;
        end_ = pending;
    }

private:
    hope<read_result> stream_buffered(
        byte_writer & writer,
        std::size_t limit)
    {
        auto n = std::min(limit, buffered_size());
        if (n == 0)
            return hope<read_result>::ready(read_result{});

        auto write = writer.write(buffered().first(n));
        if (write.is_ready()) {
            toss(n);
            return hope<read_result>::ready(read_result{.bytes = n});
        }
        return stream_buffered_slow(std::move(write), n);
    }

    static std::optional<std::size_t> first_nonempty_index(
        std::span<std::span<std::byte>> dsts) noexcept
    {
        for (auto i = std::size_t{0}; i < dsts.size(); ++i) {
            if (!dsts[i].empty())
                return i;
        }
        return std::nullopt;
    }

    std::size_t copy_buffered_to(std::span<std::span<std::byte>> dsts)
    {
        auto total = std::size_t{0};
        for (auto dst : dsts) {
            if (dst.empty())
                continue;
            auto n = std::min(dst.size(), buffered_size());
            if (n == 0)
                break;
            std::memcpy(dst.data(), buffered().data(), n);
            toss(n);
            total += n;
        }
        return total;
    }

    hope<read_result> read_more()
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw buffer_error{"reader buffer is full"};

        auto dsts = std::array{dst};
        auto result = read_vec_more(std::span{dsts});
        if (result.is_ready())
            return hope<read_result>::ready(finish_read(result.take_ready()));
        return read_more_slow(dst.size());
    }

    read_result finish_fixed_write(
        fixed_byte_writer & writer,
        read_result result)
    {
        auto written = writer.buffered_size();
        if (written != result.bytes)
            throw buffer_error{"reader stream byte count mismatch"};
        return result;
    }

    read_result finish_read(read_result result)
    {
        if (result.bytes > unused_capacity().size())
            throw buffer_error{"source overfilled read buffer"};
        end_ += result.bytes;
        return result;
    }

    hope<read_result> fill_more_without_rebase()
    {
        if (unused_capacity().empty())
            throw buffer_error{"reader buffer is full"};
        return read_more();
    }

    task<> fill_slow(std::size_t n)
    {
        rebase(n);
        while (buffered_size() < n) {
            auto read = co_await fill_more_without_rebase();
            if (read.eof && read.bytes == 0)
                throw end_of_stream{"unexpected end of input"};
        }
    }

    task<read_result> read_more_slow(std::size_t limit)
    {
        auto dsts = std::array{unused_capacity().first(limit)};
        auto result = co_await read_vec_more(std::span{dsts});
        co_return finish_read(result);
    }

    task<read_result> stream_buffered_slow(
        hope<void> write,
        std::size_t n)
    {
        co_await std::move(write);
        toss(n);
        co_return read_result{.bytes = n};
    }

    task<read_result> discard_more_slow(std::size_t limit)
    {
        auto writer = discarding_byte_writer{buffer_};
        co_return co_await stream_more(writer, limit);
    }

    task<read_result> read_vec_more_slow(std::span<std::byte> dst)
    {
        auto writer = fixed_byte_writer{dst};
        auto result = co_await stream_more(writer, dst.size());
        co_return finish_fixed_write(writer, result);
    }

    task<std::span<const std::byte>> peek_slow(std::size_t n)
    {
        co_await fill(n);
        co_return buffered().first(n);
    }

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    task<T> peek_struct_slow(hope<std::span<const std::byte>> bytes)
    {
        co_return copy_struct<T>(co_await std::move(bytes));
    }

    task<std::optional<std::span<const std::byte>>>
    take_some_slow(
        hope<read_result> first_read,
        std::size_t limit)
    {
        auto read = co_await std::move(first_read);
        if (read.eof && read.bytes == 0)
            co_return std::nullopt;
        if (read.bytes == 0)
            co_return buffered().first(0);

        auto n = std::min(limit, buffered_size());
        auto out = buffered().first(n);
        toss(n);
        co_return out;
    }

    task<std::span<const std::byte>> take_slow(std::size_t n)
    {
        auto out = co_await peek(n);
        toss(n);
        co_return out;
    }

    task<std::string_view>
    take_string_view_slow(hope<std::span<const std::byte>> bytes)
    {
        co_return as_string_view(co_await std::move(bytes));
    }

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    task<std::optional<T>> take_struct_slow()
    {
        if (buffered_size() < sizeof(T)) {
            rebase(sizeof(T));
            while (buffered_size() < sizeof(T)) {
                auto read = co_await fill_more_without_rebase();
                if (read.eof && read.bytes == 0) {
                    if (buffered_size() == 0)
                        co_return std::nullopt;
                    throw end_of_stream{"unexpected end of input"};
                }
            }
        }

        auto value = copy_struct<T>(buffered());
        toss(sizeof(T));
        co_return value;
    }

    template<typename T>
    static T copy_struct(std::span<const std::byte> bytes)
    {
        auto value = T{};
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }

    task<std::span<const std::byte>>
    take_until_slow(std::span<const std::byte> delimiter)
    {
        while (true) {
            auto available = buffered();
            auto cut = find_bytes(available, delimiter);
            if (cut < available.size()) {
                auto out = available.first(cut);
                seek_ += cut + delimiter.size();
                co_return out;
            }

            if (buffered_size() == buffer_.size())
                throw buffer_error{"reader buffer filled before delimiter"};

            auto read = co_await fill_more();
            if (read.eof && read.bytes == 0)
                throw end_of_stream{"unexpected end of input"};
        }
    }

    std::vector<std::byte> owned_buffer_;
    std::span<std::byte> buffer_;
    std::size_t seek_ = 0;
    std::size_t end_ = 0;
};

/// Reader backed by a callable returning `task<read_result>` or
/// `task<std::size_t>`. Count-only reads treat zero bytes as EOF.
template<byte_read_task Read>
class task_byte_source final : public byte_reader
{
public:
    task_byte_source(Read read, std::span<std::byte> buffer)
        : byte_reader(buffer)
        , read_(std::move(read))
    {}

    explicit task_byte_source(Read read, std::size_t buffer_size = 4096)
        : byte_reader(buffer_size)
        , read_(std::move(read))
    {}

private:
    hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) override
    {
        return stream_more_task(writer, limit);
    }

    task<read_result> stream_more_task(
        byte_writer & writer,
        std::size_t limit)
    {
        if (limit == 0)
            co_return read_result{};

        auto dst = writer.unused_capacity();
        if (!dst.empty()) {
            auto result = co_await std::invoke(
                read_,
                dst.first(std::min(dst.size(), limit)));
            auto out = normalize_result(result);
            if (out.bytes > dst.size())
                throw buffer_error{"source overfilled writer buffer"};
            writer.advance(out.bytes);
            co_return out;
        }

        rebase(1);
        auto storage = unused_capacity().first(
            std::min(unused_capacity().size(), limit));
        auto result = co_await std::invoke(read_, storage);
        auto out = normalize_result(result);
        if (out.bytes > storage.size())
            throw buffer_error{"source overfilled reader buffer"};
        advance(out.bytes);
        co_return read_result{.bytes = 0, .eof = out.eof && out.bytes == 0};
    }

    template<typename Result>
    static read_result normalize_result(Result result)
    {
        if constexpr (std::same_as<Result, read_result>) {
            return result;
        } else {
            return read_result{
                .bytes = result,
                .eof = result == 0,
            };
        }
    }

    Read read_;
};

/// Borrowed in-memory byte reader over a single-pass range of byte-like chunks.
///
/// Chunks may be byte spans or UTF-8 text views; text chunks are treated as
/// their underlying bytes.
template<std::ranges::input_range Chunks>
    requires std::ranges::view<Chunks>
        && detail::byte_reader_chunk_range<Chunks>
class byte_span_source final : public byte_reader
{
public:
    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Chunks, std::views::all_t<Range>>
            && detail::byte_reader_chunk_range<std::views::all_t<Range>>
    byte_span_source(Range && chunks, std::span<std::byte> buffer)
        : byte_reader(buffer)
        , chunks_(std::views::all(std::forward<Range>(chunks)))
        , chunk_(std::ranges::begin(chunks_))
        , end_(std::ranges::end(chunks_))
    {}

    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Chunks, std::views::all_t<Range>>
            && detail::byte_reader_chunk_range<std::views::all_t<Range>>
    explicit byte_span_source(
        Range && chunks,
        std::size_t buffer_size = 4096)
        : byte_reader(buffer_size)
        , chunks_(std::views::all(std::forward<Range>(chunks)))
        , chunk_(std::ranges::begin(chunks_))
        , end_(std::ranges::end(chunks_))
    {}

private:
    hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<read_result>::ready(read_result{});

        auto total = std::size_t{0};
        auto remaining = limit;
        while (chunk_ != end_) {
            auto chunk = detail::reader_chunk_bytes(*chunk_);
            auto rest = chunk.subspan(offset_);
            if (rest.empty()) {
                ++chunk_;
                offset_ = 0;
                continue;
            }

            auto n = std::min(remaining, rest.size());
            auto dst = writer.unused_capacity();
            if (!dst.empty())
                n = std::min(n, dst.size());

            auto write = writer.write(rest.first(n));
            if (write.is_ready()) {
                advance_chunk(n, chunk.size());
                total += n;
                remaining -= n;
                if (remaining == 0 || writer.unused_capacity().empty())
                    return hope<read_result>::ready(
                        read_result{.bytes = total, .eof = chunk_ == end_});
                continue;
            }

            return stream_write_slow(std::move(write), n, chunk.size(), total);
        }

        return hope<read_result>::ready(
            read_result{.bytes = total, .eof = true});
    }

    task<read_result> stream_write_slow(
        hope<void> write,
        std::size_t n,
        std::size_t chunk_size,
        std::size_t prefix)
    {
        co_await std::move(write);
        advance_chunk(n, chunk_size);
        co_return read_result{.bytes = prefix + n, .eof = chunk_ == end_};
    }

    void advance_chunk(std::size_t n, std::size_t chunk_size)
    {
        offset_ += n;
        if (offset_ == chunk_size) {
            ++chunk_;
            offset_ = 0;
        }
    }

    Chunks chunks_;
    std::ranges::iterator_t<Chunks> chunk_;
    std::ranges::sentinel_t<Chunks> end_;
    std::size_t offset_ = 0;
};

template<std::ranges::viewable_range Range>
    requires detail::byte_reader_chunk_range<std::views::all_t<Range>>
byte_span_source(Range &&, std::span<std::byte>)
    -> byte_span_source<std::views::all_t<Range>>;

template<std::ranges::viewable_range Range>
    requires detail::byte_reader_chunk_range<std::views::all_t<Range>>
byte_span_source(Range &&)
    -> byte_span_source<std::views::all_t<Range>>;

template<std::ranges::viewable_range Range>
    requires detail::byte_reader_chunk_range<std::views::all_t<Range>>
byte_span_source(Range &&, std::size_t)
    -> byte_span_source<std::views::all_t<Range>>;

/// Reader for a file descriptor.
class fd_source final : public byte_reader
{
public:
    explicit fd_source(int fd, std::span<std::byte> buffer)
        : byte_reader(buffer)
        , fd_(fd)
    {}

    explicit fd_source(int fd, std::size_t buffer_size = 4096)
        : byte_reader(buffer_size)
        , fd_(fd)
    {}

private:
    hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) override
    {
        return stream_more_task(writer, limit);
    }

    task<read_result> stream_more_task(
        byte_writer & writer,
        std::size_t limit)
    {
        if (limit == 0)
            co_return read_result{};

        auto dst = writer.unused_capacity();
        if (dst.empty()) {
            rebase(1);
            dst = unused_capacity().first(
                std::min(unused_capacity().size(), limit));
            auto n = co_await read_some(dst);
            advance(n);
            co_return read_result{.bytes = 0, .eof = n == 0};
        }

        auto n = co_await read_some(dst.first(std::min(dst.size(), limit)));
        writer.advance(n);
        co_return read_result{
            .bytes = n,
            .eof = n == 0,
        };
    }

    task<std::size_t> read_some(std::span<std::byte> dst)
    {
        while (true) {
            try {
                co_return co_await op::read_some{
                    .fd = fd_,
                    .buffer = dst,
                    .offset = -1,
                };
            } catch (const interrupted_system_call &) {
            }
        }
    }

    int fd_ = -1;
};

/// Reader for a connected socket.
class socket_source final : public byte_reader
{
public:
    explicit socket_source(
        int fd,
        std::span<std::byte> buffer,
        int flags = 0)
        : byte_reader(buffer)
        , fd_(fd)
        , flags_(flags)
    {}

    explicit socket_source(
        int fd,
        int flags = 0,
        std::size_t buffer_size = 4096)
        : byte_reader(buffer_size)
        , fd_(fd)
        , flags_(flags)
    {}

private:
    hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) override
    {
        return stream_more_task(writer, limit);
    }

    task<read_result> stream_more_task(
        byte_writer & writer,
        std::size_t limit)
    {
        if (limit == 0)
            co_return read_result{};

        auto dst = writer.unused_capacity();
        if (dst.empty()) {
            rebase(1);
            dst = unused_capacity().first(
                std::min(unused_capacity().size(), limit));
            auto n = co_await recv_some(dst);
            advance(n);
            co_return read_result{.bytes = 0, .eof = n == 0};
        }

        auto n = co_await recv_some(dst.first(std::min(dst.size(), limit)));
        writer.advance(n);
        co_return read_result{
            .bytes = n,
            .eof = n == 0,
        };
    }

    task<std::size_t> recv_some(std::span<std::byte> dst)
    {
        while (true) {
            try {
                co_return co_await op::recv_some{
                    .fd = fd_,
                    .buffer = dst,
                    .flags = flags_,
                };
            } catch (const interrupted_system_call &) {
            }
        }
    }

    int fd_ = -1;
    int flags_ = 0;
};

class metered_byte_source final : public byte_reader
{
public:
    metered_byte_source(
        byte_reader & inner,
        std::span<std::byte> buffer,
        std::function<void(std::size_t)> progress)
        : byte_reader(buffer)
        , inner_(inner)
        , progress_(std::move(progress))
    {}

    metered_byte_source(
        byte_reader & inner,
        std::function<void(std::size_t)> progress,
        std::size_t buffer_size = 4096)
        : byte_reader(buffer_size)
        , inner_(inner)
        , progress_(std::move(progress))
    {}

private:
    hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) override
    {
        auto streamed = inner_.stream(writer, limit);
        if (streamed.is_ready())
            return hope<read_result>::ready(meter(streamed.take_ready()));
        return stream_more_slow(std::move(streamed));
    }

    task<read_result> stream_more_slow(hope<read_result> streamed)
    {
        co_return meter(co_await std::move(streamed));
    }

    read_result meter(read_result result)
    {
        if (result.bytes != 0)
            progress_(result.bytes);
        return result;
    }

    byte_reader & inner_;
    std::function<void(std::size_t)> progress_;
};

inline metered_byte_source meter_source(
    byte_reader & inner,
    std::function<void(std::size_t)> progress,
    std::size_t buffer_size = 4096)
{
    return metered_byte_source{inner, std::move(progress), buffer_size};
}

/// Repeatedly consume the reader's buffered chunks and visit each one.
///
/// `visitor` is called synchronously with the bytes read before the next read
/// is posted. The chunk span is only valid until the next loop iteration.
template<typename Visitor>
task<std::size_t> for_each_chunk(
    byte_reader & reader,
    Visitor visitor)
{
    auto total = std::size_t{0};
    while (true) {
        auto chunk = co_await reader.take_some();
        if (!chunk)
            co_return total;

        visitor(*chunk);
        total += chunk->size();
    }
}

/// Stream all bytes from `reader` into `writer`, then flush `writer`.
///
/// Returns the number of bytes accepted by the writer. Concrete writers that
/// count actual backend writes may expose their own count after `flush()`.
inline task<std::size_t> stream_all(
    byte_reader & reader,
    byte_writer & writer)
{
    auto total = std::size_t{0};
    while (true) {
        auto result = co_await reader.stream(writer);
        total += result.bytes;
        if (result.eof)
            break;
    }
    co_await writer.flush();
    co_return total;
}

} // namespace nxtrt
