#pragma once

#include "nxtrt/buffer-core.hpp"
#include "nxtrt/exceptions.hpp"
#include "nxtrt/task.hpp"
#include "nxtrt/value-buffers.hpp"
#include "nxtrt/wish_ops.hpp"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace nxtrt {

template<typename Read>
concept byte_read_task = detail::value_read_task<std::byte, Read>;

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
concept bytesink_chunk =
    std::same_as<std::remove_cvref_t<T>, std::string>
    || std::convertible_to<T, std::string_view>
    || std::convertible_to<T, std::span<const std::byte>>;

template<typename T>
concept bytesink_chunk_range =
    std::ranges::input_range<T>
    && (!bytesink_chunk<T>)
    && bytesink_chunk<std::ranges::range_reference_t<T>>;

template<typename T>
concept bytefeed_chunk =
    std::convertible_to<T, std::span<const std::byte>>
    || (
        std::convertible_to<T, std::string_view>
        && (
            !std::same_as<std::remove_cvref_t<T>, std::string>
            || std::is_lvalue_reference_v<T>));

template<typename T>
concept bytefeed_chunk_range =
    std::ranges::input_range<T>
    && bytefeed_chunk<std::ranges::range_reference_t<T>>;

template<bytefeed_chunk Chunk>
std::span<const std::byte> feed_chunk_bytes(Chunk && chunk) noexcept
{
    if constexpr (std::convertible_to<Chunk, std::span<const std::byte>>) {
        return std::span<const std::byte>{std::forward<Chunk>(chunk)};
    } else {
        return as_bytes(std::string_view{std::forward<Chunk>(chunk)});
    }
}

template<bytesink_chunk Chunk>
std::span<const std::byte> sink_chunk_bytes(Chunk && chunk) noexcept
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

using bytesink = sink<std::byte>;

template<detail::bytesink_chunk_range Chunks>
task<void> write(bytesink & writer, Chunks && chunks)
{
    for (auto && chunk : chunks)
        co_await writer.write(
            detail::sink_chunk_bytes(std::forward<decltype(chunk)>(chunk)));
}

/// Writer for a file descriptor.
class fd_sink final : public bytesink
{
public:
    explicit fd_sink(int fd, std::span<std::byte> buffer)
        : bytesink(buffer)
        , fd_(fd)
    {}

    explicit fd_sink(int fd, std::size_t buffer_size = 4096)
        : bytesink(buffer_size)
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

inline fd_sink standard_output_sink(std::size_t buffer_size = 4096)
{
    return standard_output(buffer_size);
}

/// Writer for a connected socket.
class socket_sink final : public bytesink
{
public:
    explicit socket_sink(
        int fd,
        std::span<std::byte> buffer,
        int flags = 0)
        : bytesink(buffer)
        , fd_(fd)
        , flags_(flags)
    {}

    explicit socket_sink(
        int fd,
        int flags = 0,
        std::size_t buffer_size = 4096)
        : bytesink(buffer_size)
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
        || detail::bytesink_chunk_range<Chunks>
inline task<void> write_all(bytesink & writer, Chunks && chunks)
{
    co_await write(writer, std::forward<Chunks>(chunks));
    co_await writer.flush();
}

// ===========================================================================
// Design note: Zig std.Io and nxt feeds/sinks
// ===========================================================================
//
// This feed/sink family is a deliberate port of Zig's post-0.15
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
// that is what lets a file feed splice straight into a socket sink via
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
// The mandatory feed cold path is now `stream_more()`, the Zig-shaped verb:
// push up to `limit` bytes into a `bytesink`. Like Zig, an implementation may
// also choose to put bytes in its own reader buffer and return zero streamed
// bytes; the next public `stream()`/`take()` call will consume those buffered
// bytes through the hot path. Pull-shaped refill is derived by pointing a fixed
// sink at the feed's unused capacity, so source bytes still land in
// `buffer_` before borrowed-span APIs (`peek`/`take`) expose them. This gives us
// the structure of Zig's "stream into a sink, and refill is just streaming into
// my own buffer" without yet caring about fd-to-fd sendfile-style optimization.
//
// The remaining divergence is that nxt readers still require at least one byte
// of storage. Zig can express "fill my Reader.buffer" through `readVec`'s
// special empty-slice convention; our concrete refill has nowhere to put source
// bytes unless the reader owns or borrows a real span, even if it is only one
// byte. A zero-buffer bytefeed can make sense once the cold surface is rich
// enough to avoid pull-shaped refill entirely, but today's `peek`/`take` APIs
// need a place to park bytes.
//
// The hot/cold split is mirrored by `hope<T>` (see task.hpp). The buffered case
// returns `hope<...>::ready(span)` -- a synchronous value, no coroutine frame,
// no deck round-trip -- and only a miss builds a `*_slow` task that is spliced
// as the awaiter continuation. This is the C++ stackless answer to Zig's
// `fill`/`fillUnbuffered` inlining trick: without it, `co_await feed.take(n)`
// on already-buffered data would still bounce the deck (lazy `task<T>` is never
// `await_ready`), which is exactly the per-field trampoline this design exists
// to kill.
//
// `stream_more()` itself returns `hope<fare_t>`, not `task<fare_t>`.
// That is the payoff lever: a layer that already holds bytes (decrypted TLS
// plaintext, an in-memory span) streams or refills SYNCHRONOUSLY, so a
// fully-buffered read composes with zero suspensions through a whole stack of
// feeds (socket -> tls -> http_body -> sse). It is the "eager wish" idea
// applied to the cold verb, achieved without any wand change.
//
// The wider Zig vocabulary is now structural: `stream()` and `read_vec()` use
// buffered bytes when they have them and otherwise call the corresponding cold
// virtual (`stream_more()` / `read_vec_more()`). `discard()` uses buffered bytes
// when it has them and otherwise calls overridable `discard_more()`. Refill is
// just `read_vec_more({unused_capacity()})`, i.e. readVec into this reader's
// own buffer.
//
// On the sink side we intentionally one-up Zig a little: `bytesink` has a
// feed-like `seek_` as well as `end_`, so partially drained buffered output
// is represented honestly as `buffer_[seek_..end_]`. That makes Zig-style
// `rebase(preserve, capacity)` direct: drain only the non-preserved prefix,
// keep the recent suffix staged, and compact when contiguous capacity is needed.
// Zig's writer source has a TODO wishing for this because its default rebase
// logic temporarily hides preserved bytes by mutating `end`.
//
// --- What we still owe to fully adopt the paradigm --------------------------
//
// TODO(zig-stream): teach concrete fd/socket feeds and sinks about each
//   other so `stream_more()` can eventually use sendfile/readv/writev-shaped
//   paths. Today it has the right API shape but still moves ordinary spans.
// TODO(zig-readvec): teach fd/socket/task-backed sources real scatter reads.
//   The virtual slot exists, but the generic default still streams into only
//   the first non-empty destination, matching Zig's simple default.
// TODO(zig-discard): optimized `discard_more(limit)` overrides so protocols can
//   skip bytes (chunked trailers, body skip-to-end) without buffering and
//   copying them through a sink-shaped shim.
// TODO(zig-rebase): use the virtual `rebase_more` slot for a ring- or
//   mmap-backed reader that can make room differently from memmove.
// TODO(eager-wand): push the synchronous-completion idea of `stream_more()` down
//   to the wish layer -- an honest `urge::await_ready()` plus a sync path in
//   `wand::prepare` -- so a warm `read_some` on the fd also skips the
//   round-trip. At that point the buffered feed can BE a wand and `hope`
//   dissolves into a single "maybe already here, else suspends" awaitable
//   shared by wishes and readers alike. That is the endgame this whole family
//   is shaped toward.

using bytefeed = feed<std::byte>;

/// Reader backed by a callable returning `task<fare_t>` or
/// `task<std::size_t>`. Count-only reads treat zero bytes as EOF.
template<byte_read_task Read>
class task_bytefeed final : public taskfeed<std::byte, Read>
{
    using base = taskfeed<std::byte, Read>;

public:
    using base::base;
};

/// Borrowed in-memory byte reader over a single-pass range of byte-like chunks.
///
/// Chunks may be byte spans or UTF-8 text views; text chunks are treated as
/// their underlying bytes.
template<std::ranges::input_range Chunks>
    requires std::ranges::view<Chunks>
        && detail::bytefeed_chunk_range<Chunks>
class byte_span_feed final : public bytefeed
{
public:
    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Chunks, std::views::all_t<Range>>
            && detail::bytefeed_chunk_range<std::views::all_t<Range>>
    byte_span_feed(Range && chunks, std::span<std::byte> buffer)
        : bytefeed(buffer)
        , chunks_(std::views::all(std::forward<Range>(chunks)))
        , chunk_(std::ranges::begin(chunks_))
        , end_(std::ranges::end(chunks_))
    {}

    template<std::ranges::viewable_range Range>
        requires std::constructible_from<Chunks, std::views::all_t<Range>>
            && detail::bytefeed_chunk_range<std::views::all_t<Range>>
    explicit byte_span_feed(
        Range && chunks,
        std::size_t buffer_size = 4096)
        : bytefeed(buffer_size)
        , chunks_(std::views::all(std::forward<Range>(chunks)))
        , chunk_(std::ranges::begin(chunks_))
        , end_(std::ranges::end(chunks_))
    {}

private:
    hope<fare_t> stream_more(
        bytesink & writer,
        std::size_t limit) override
    {
        if (limit == 0)
            return hope<fare_t>::ready(0);

        auto total = std::size_t{0};
        auto remaining = limit;
        while (chunk_ != end_) {
            auto chunk = detail::feed_chunk_bytes(*chunk_);
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
                    return hope<fare_t>::ready(
                        total);
                continue;
            }

            return stream_write_slow(std::move(write), n, chunk.size(), total);
        }

        if (total == 0)
            return hope<fare_t>::ready(eof);
        return hope<fare_t>::ready(total);
    }

    task<fare_t> stream_write_slow(
        hope<void> write,
        std::size_t n,
        std::size_t chunk_size,
        std::size_t prefix)
    {
        co_await std::move(write);
        advance_chunk(n, chunk_size);
        co_return prefix + n;
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
    requires detail::bytefeed_chunk_range<std::views::all_t<Range>>
byte_span_feed(Range &&, std::span<std::byte>)
    -> byte_span_feed<std::views::all_t<Range>>;

template<std::ranges::viewable_range Range>
    requires detail::bytefeed_chunk_range<std::views::all_t<Range>>
byte_span_feed(Range &&)
    -> byte_span_feed<std::views::all_t<Range>>;

template<std::ranges::viewable_range Range>
    requires detail::bytefeed_chunk_range<std::views::all_t<Range>>
byte_span_feed(Range &&, std::size_t)
    -> byte_span_feed<std::views::all_t<Range>>;

/// Reader for a file descriptor.
class fd_source final : public detail::taskfeed_base<std::byte, fd_source>
{
    using base = detail::taskfeed_base<std::byte, fd_source>;

public:
    explicit fd_source(int fd, std::span<std::byte> buffer)
        : base(buffer)
        , fd_(fd)
    {}

    explicit fd_source(int fd, std::size_t buffer_size = 4096)
        : base(buffer_size)
        , fd_(fd)
    {}

    task<std::size_t> read_into(junk<std::byte> dst)
    {
        while (true) {
            try {
                co_return co_await op::read_some{
                    .fd = fd_,
                    .buffer = dst.as_writable_bytes(),
                    .offset = -1,
                };
            } catch (const interrupted_system_call &) {
            }
        }
    }

private:
    friend base;

    int fd_ = -1;
};

/// Reader for a connected socket.
class socket_source final : public detail::taskfeed_base<std::byte, socket_source>
{
    using base = detail::taskfeed_base<std::byte, socket_source>;

public:
    explicit socket_source(
        int fd,
        std::span<std::byte> buffer,
        int flags = 0)
        : base(buffer)
        , fd_(fd)
        , flags_(flags)
    {}

    explicit socket_source(
        int fd,
        int flags = 0,
        std::size_t buffer_size = 4096)
        : base(buffer_size)
        , fd_(fd)
        , flags_(flags)
    {}

    /// Bytes successfully received from the socket by completed reads.
    [[nodiscard]] std::size_t received_size() const noexcept
    {
        return received_;
    }

    task<std::size_t> read_into(junk<std::byte> dst)
    {
        while (true) {
            try {
                auto n = co_await op::recv_some{
                    .fd = fd_,
                    .buffer = dst.as_writable_bytes(),
                    .flags = flags_,
                };
                received_ += n;
                co_return n;
            } catch (const interrupted_system_call &) {
            }
        }
    }

private:
    friend base;

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
    bytefeed & reader,
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
    bytefeed & reader,
    bytesink & writer)
{
    auto total = std::size_t{0};
    while (true) {
        auto result = co_await reader.stream(writer);
        total += value_count(result);
        if (is_eof(result))
            break;
    }
    co_await writer.flush();
    co_return total;
}

} // namespace nxtrt
