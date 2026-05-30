#pragma once

#include "nxtrt/buffer-core.hpp"
#include "nxtrt/task.hpp"
#include "nxtrt/value-buffers.hpp"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nxtrt {

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

template<byte_writer_chunk Chunk>
std::span<const std::byte> writer_chunk_bytes(Chunk && chunk) noexcept
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

using byte_writer = value_sink<std::byte>;

template<detail::byte_writer_chunk_range Chunks>
task<void> write(byte_writer & writer, Chunks && chunks)
{
    for (auto && chunk : chunks)
        co_await writer.write(
            detail::writer_chunk_bytes(std::forward<decltype(chunk)>(chunk)));
}

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
        value_chunk_view,
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
        value_chunk_view chunks,
        std::size_t splat) override
    {
        return hope<std::size_t>::ready(byte_size(chunks, splat));
    }

    static std::size_t byte_size(value_chunk_view chunks, std::size_t splat)
    {
        if (chunks.empty())
            return 0;

        auto total = std::size_t{0};
        auto spans = chunks.chunks();
        for (auto chunk : spans.first(spans.size() - 1))
            total += chunk.size();
        total += spans.back().size() * splat;
        return total;
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

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override
    {
        return drain_more_task(chunks, splat);
    }

    task<std::size_t>
    drain_more_task(
        value_chunk_view chunks,
        std::size_t splat)
    {
        auto src = first_nonempty(chunks, splat);
        while (true) {
            try {
                co_return co_await op::write_some{
                    .fd = fd_,
                    .buffer = src,
                    .offset = -1,
                };
            } catch (const interrupted_system_call &) {
            }
        }
    }

    static std::span<const std::byte> first_nonempty(
        value_chunk_view chunks,
        std::size_t splat) noexcept
    {
        if (chunks.empty())
            return {};
        auto spans = chunks.chunks();
        for (auto chunk : spans.first(spans.size() - 1)) {
            if (!chunk.empty())
                return chunk;
        }
        if (splat != 0 && !spans.back().empty())
            return spans.back();
        return {};
    }

    int fd_ = -1;
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

    /// Bytes successfully sent by completed socket drains.
    [[nodiscard]] std::size_t sent_size() const noexcept
    {
        return sent_;
    }

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override
    {
        return drain_more_task(chunks, splat);
    }

    task<std::size_t>
    drain_more_task(
        value_chunk_view chunks,
        std::size_t splat)
    {
        auto src = first_nonempty(chunks, splat);
        while (true) {
            try {
                auto n = co_await op::send_some{
                    .fd = fd_,
                    .buffer = src,
                    .flags = flags_,
                };
                sent_ += n;
                co_return n;
            } catch (const interrupted_system_call &) {
            }
        }
    }

    static std::span<const std::byte> first_nonempty(
        value_chunk_view chunks,
        std::size_t splat) noexcept
    {
        if (chunks.empty())
            return {};
        auto spans = chunks.chunks();
        for (auto chunk : spans.first(spans.size() - 1)) {
            if (!chunk.empty())
                return chunk;
        }
        if (splat != 0 && !spans.back().empty())
            return spans.back();
        return {};
    }

    int fd_ = -1;
    int flags_ = 0;
    std::size_t sent_ = 0;
};

template<typename Chunks>
    requires std::convertible_to<Chunks, std::string_view>
        || std::convertible_to<Chunks, std::span<const std::byte>>
        || detail::byte_writer_chunk_range<Chunks>
inline task<void> write_all(byte_writer & writer, Chunks && chunks)
{
    co_await write(writer, std::forward<Chunks>(chunks));
    co_await writer.flush();
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
class byte_reader : public value_source<std::byte>
{
public:
    using byte_chunk_view = byte_chunks<std::byte>;
    using const_byte_chunk_view = byte_chunks<const std::byte>;
    using value_source<std::byte>::buffered_size;
    using value_source<std::byte>::unused_capacity_size;

    /// Construct a reader with owned buffer storage.
    explicit byte_reader(std::size_t buffer_size)
        : value_source<std::byte>(require_nonzero(buffer_size))
    {}

    /// Construct a reader over caller-owned buffer storage.
    ///
    /// The storage must outlive the reader.
    explicit byte_reader(std::span<std::byte> buffer)
        : value_source<std::byte>(require_nonempty(buffer))
    {}

    virtual ~byte_reader() = default;

    byte_reader(const byte_reader &) = delete;
    byte_reader & operator=(const byte_reader &) = delete;

    byte_reader(byte_reader &&) = delete;
    byte_reader & operator=(byte_reader &&) = delete;

    /// Return currently buffered bytes.
    ///
    /// The returned span is invalidated by any operation that refills, rebases,
    /// or consumes the reader.
    [[nodiscard]] std::span<const std::byte> buffered() const
    {
        auto one = buffered_chunks().single_span();
        if (!one)
            throw buffer_error{"reader buffered bytes are wrapped"};
        return *one;
    }

    /// Return currently buffered bytes as chunks.
    ///
    /// This currently mirrors `buffered()` as one chunk, and is the shared shape
    /// used by ring-buffered value sources and sinks.
    [[nodiscard]] const_byte_chunk_view buffered_chunks() const noexcept
    {
        return value_source<std::byte>::buffered();
    }

    /// Return writable storage after the currently buffered bytes.
    ///
    /// Derived readers may write source bytes here and call protected
    /// `advance()`.
    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return value_source<std::byte>::unused_capacity();
    }

    /// Ensure that `capacity` bytes can fit from the current seek position.
    ///
    /// The default cold path memmoves buffered data to the start of the buffer.
    /// Derived readers may override `rebase_more()` for ring buffers or other
    /// storage strategies.
    void rebase(std::size_t capacity)
    {
        if (capacity > storage_capacity())
            throw buffer_error{"reader buffer is too small"};
        rebase_more(capacity);
    }

    /// Ensure at least `n` bytes are buffered.
    ///
    /// If the bytes are already present this returns ready and does not suspend.
    /// If EOF occurs before `n` bytes are available, throws `end_of_stream`.
    hope<void> fill(std::size_t n)
    {
        if (n > storage_capacity())
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
        if (buffered_size() == storage_capacity())
            throw buffer_error{"reader buffer is full"};
        return read_more();
    }

    /// Borrow the next `n` bytes without consuming them.
    ///
    /// The returned span remains valid only until the next operation that may
    /// mutate the reader buffer.
    hope<std::span<const std::byte>> peek(std::size_t n)
    {
        if (n > storage_capacity())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n) {
            auto chunks = buffered_chunks().first(n);
            auto one = chunks.single_span();
            if (one)
                return hope<std::span<const std::byte>>::ready(*one);
            contiguize_buffered_for_derived();
            return hope<std::span<const std::byte>>::ready(
                buffered().first(n));
        }

        return peek_slow(n);
    }

    /// Borrow the next `n` bytes as chunks without consuming them.
    ///
    /// This is the ring-buffer-friendly counterpart to `peek(n)`.
    hope<const_byte_chunk_view> peek_chunks(std::size_t n)
    {
        if (n > storage_capacity())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n)
            return hope<const_byte_chunk_view>::ready(
                buffered_chunks().first(n));
        return peek_chunks_slow(n);
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

        auto out = first_buffered_span(limit);
        auto n = out.size();
        toss(n);
        return hope<std::optional<std::span<const std::byte>>>::ready(out);
    }

    /// Discard `n` already-buffered bytes.
    void toss(std::size_t n)
    {
        try {
            consume_buffered_for_derived(n);
        } catch (const value_buffer_error &) {
            throw buffer_error{"reader consumed past buffered input"};
        }
    }

    /// Consume and return exactly `n` borrowed bytes.
    ///
    /// Throws `buffer_error` if `n` is larger than this reader's whole buffer,
    /// and `end_of_stream` if EOF arrives before `n` bytes are available.
    hope<std::span<const std::byte>> take(std::size_t n)
    {
        if (n > storage_capacity())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n) {
            auto out = coerce_buffered_span(n);
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
            auto value = copy_struct<T>(buffered_chunks().first(sizeof(T)));
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

        contiguize_buffered_for_derived();
        auto available = buffered();
        auto cut = find_bytes(available, delimiter);
        if (cut < available.size()) {
            auto out = available.first(cut);
            toss(cut + delimiter.size());
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
            return stream_bytes_more(writer, limit);

        return stream_buffered(writer, limit);
    }

    /// Discard one available chunk, up to `limit` bytes.
    ///
    /// Buffered bytes are tossed synchronously. Otherwise this delegates to
    /// `discard_bytes_more()`, whose default implementation streams into a
    /// discarding writer.
    hope<read_result> discard(
        std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (limit == 0)
            return hope<read_result>::ready(read_result{});

        if (buffered_size() == 0)
            return discard_bytes_more(limit);

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
        return value_source<std::byte>::buffer_storage();
    }

    /// Commit `n` bytes that a derived reader wrote into `unused_capacity()`.
    void advance(std::size_t n)
    {
        try {
            advance_constructed(n);
        } catch (const value_buffer_error &) {
            throw buffer_error{"reader advanced past buffer capacity"};
        }
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
    virtual hope<read_result> stream_bytes_more(
        byte_writer & writer,
        std::size_t limit) = 0;

    hope<value_result> stream_more(
        value_sink<std::byte> & sink,
        std::size_t limit) override
    {
        auto result = stream_bytes_more(sink, limit);
        if (result.is_ready())
            return hope<value_result>::ready(
                to_value_result(result.take_ready()));
        return stream_more_value_slow(std::move(result));
    }

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
        auto result = stream_bytes_more(writer, dst.size());
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
    virtual hope<read_result> discard_bytes_more(std::size_t limit)
    {
        auto writer = discarding_byte_writer{buffer_storage()};
        auto result = stream_bytes_more(writer, limit);
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
        contiguize_buffered_for_derived();
    }

private:
    hope<read_result> stream_buffered(
        byte_writer & writer,
        std::size_t limit)
    {
        auto bytes = first_buffered_span(limit);
        auto n = bytes.size();
        if (n == 0)
            return hope<read_result>::ready(read_result{});

        auto write = writer.write(bytes);
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
            auto src = first_buffered_span(n);
            n = src.size();
            std::memcpy(dst.data(), src.data(), n);
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
        advance(result.bytes);
        return result;
    }

    task<> fill_slow(std::size_t n)
    {
        while (buffered_size() < n) {
            auto read = co_await fill_more();
            if (read.eof && read.bytes == 0)
                throw end_of_stream{"unexpected end of input"};
        }
    }

    task<value_result> stream_more_value_slow(hope<read_result> result)
    {
        co_return to_value_result(co_await std::move(result));
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
        auto writer = discarding_byte_writer{buffer_storage()};
        co_return co_await stream_bytes_more(writer, limit);
    }

    task<read_result> read_vec_more_slow(std::span<std::byte> dst)
    {
        auto writer = fixed_byte_writer{dst};
        auto result = co_await stream_bytes_more(writer, dst.size());
        co_return finish_fixed_write(writer, result);
    }

    task<std::span<const std::byte>> peek_slow(std::size_t n)
    {
        co_await fill(n);
        co_return coerce_buffered_span(n);
    }

    task<const_byte_chunk_view> peek_chunks_slow(std::size_t n)
    {
        co_await fill(n);
        co_return buffered_chunks().first(n);
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

        auto out = first_buffered_span(limit);
        toss(out.size());
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
        try {
            auto ready = value_source<std::byte>::fill(sizeof(T));
            if (!ready.is_ready())
                co_await std::move(ready);
        } catch (const value_end_of_stream &) {
            if (buffered_size() == 0)
                co_return std::nullopt;
            throw end_of_stream{"unexpected end of input"};
        } catch (const value_buffer_error & e) {
            throw buffer_error{e.what()};
        }

        auto value = copy_struct<T>(buffered_chunks().first(sizeof(T)));
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

    template<typename T>
    static T copy_struct(const_byte_chunk_view chunks)
    {
        auto value = T{};
        auto dst = std::as_writable_bytes(std::span{&value, 1});
        auto offset = std::size_t{0};
        for (auto chunk : chunks.first(sizeof(T))) {
            std::memcpy(dst.data() + offset, chunk.data(), chunk.size());
            offset += chunk.size();
        }
        return value;
    }

    task<std::span<const std::byte>>
    take_until_slow(std::span<const std::byte> delimiter)
    {
        while (true) {
            contiguize_buffered_for_derived();
            auto available = buffered();
            auto cut = find_bytes(available, delimiter);
            if (cut < available.size()) {
                auto out = available.first(cut);
                toss(cut + delimiter.size());
                co_return out;
            }

            if (buffered_size() == storage_capacity())
                throw buffer_error{"reader buffer filled before delimiter"};

            auto read = co_await fill_more();
            if (read.eof && read.bytes == 0)
                throw end_of_stream{"unexpected end of input"};
        }
    }

    std::span<const std::byte> first_buffered_span(
        std::size_t limit = std::numeric_limits<std::size_t>::max()) const
    {
        auto chunks = buffered_chunks();
        auto first = chunks.single_span();
        if (first)
            return first->first(std::min(limit, first->size()));
        return chunks.chunks().front().first(
            std::min(limit, chunks.chunks().front().size()));
    }

    std::span<const std::byte> coerce_buffered_span(std::size_t n)
    {
        auto chunks = buffered_chunks().first(n);
        auto one = chunks.single_span();
        if (one)
            return *one;
        contiguize_buffered_for_derived();
        return buffered().first(n);
    }

    static std::size_t require_nonzero(std::size_t size)
    {
        if (size == 0)
            throw buffer_error{"reader buffer is empty"};
        return size;
    }

    static std::span<std::byte> require_nonempty(std::span<std::byte> buffer)
    {
        if (buffer.empty())
            throw buffer_error{"reader buffer is empty"};
        return buffer;
    }

    static value_result to_value_result(read_result result) noexcept
    {
        return value_result{.values = result.bytes, .eof = result.eof};
    }

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
    hope<read_result> stream_bytes_more(
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
            writer.advance_constructed(out.bytes);
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
    hope<read_result> stream_bytes_more(
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
    hope<read_result> stream_bytes_more(
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
        writer.advance_constructed(n);
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

    /// Bytes successfully received from the socket by completed reads.
    [[nodiscard]] std::size_t received_size() const noexcept
    {
        return received_;
    }

private:
    hope<read_result> stream_bytes_more(
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
        writer.advance_constructed(n);
        co_return read_result{
            .bytes = n,
            .eof = n == 0,
        };
    }

    task<std::size_t> recv_some(std::span<std::byte> dst)
    {
        while (true) {
            try {
                auto n = co_await op::recv_some{
                    .fd = fd_,
                    .buffer = dst,
                    .flags = flags_,
                };
                received_ += n;
                co_return n;
            } catch (const interrupted_system_call &) {
            }
        }
    }

    int fd_ = -1;
    int flags_ = 0;
    std::size_t received_ = 0;
};

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
