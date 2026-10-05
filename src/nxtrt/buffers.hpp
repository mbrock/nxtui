#pragma once

#include "nxtrt/buffer-core.hpp"
#include "nxtrt/exceptions.hpp"
#include "nxtrt/task.hpp"
#include "nxtrt/value-buffers.hpp"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace nxtrt {

/// A callable taking `junk<std::byte>` and returning `task<fare_t>` or
/// `task<std::size_t>`; see @ref nxtrt::taskfeed "taskfeed".
template<typename Read>
concept byte_read_task = detail::value_read_task<std::byte, Read>;

/// Send some of `buffer` on socket `fd` with one `op::send_some` wish.
/// Returns the count sent, which may be short.
task<std::size_t> send_some(
    socket_handle fd,
    std::span<const std::byte> buffer,
    int flags = 0);

/// Write some of `buffer` to `fd` with one `op::write_some` wish, at
/// `offset` or, when it is -1, at the current file position. Returns the
/// count written, which may be short.
task<std::size_t> write_some(
    io_handle fd,
    std::span<const std::byte> buffer,
    file_offset offset = -1);

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

/// Byte sink: @ref sink of `std::byte`, the writer side of byte I/O.
///
/// Text is written as its bytes with the `write(sink, std::string_view)`
/// helpers, and formatted with @ref print.
using bytesink = sink<std::byte>;

/// Write each chunk of a range of strings, string views, or byte spans.
/// Does not flush.
template<detail::bytesink_chunk_range Chunks>
task<void> write(bytesink & writer, Chunks && chunks)
{
    for (auto && chunk : chunks)
        co_await writer.write(
            detail::sink_chunk_bytes(std::forward<decltype(chunk)>(chunk)));
}

/// Buffered byte sink over a file descriptor.
///
/// Each drain is one `op::write_some` wish of the first nonempty staged
/// chunk, retried on `EINTR`; a short write leaves the rest buffered. Other
/// write errors propagate as exceptions from the awaited write or flush. The
/// fd is borrowed: the sink never closes it, and it must stay open while the
/// sink is used. Remember to `flush()` before the sink goes away. The
/// default buffer is 4096 bytes.
class fd_sink final : public bytesink
{
public:
    explicit fd_sink(io_handle fd, std::span<std::byte> buffer)
        : bytesink(buffer)
        , fd_(fd)
    {}

    explicit fd_sink(io_handle fd, std::size_t buffer_size = 4096)
        : bytesink(buffer_size)
        , fd_(fd)
    {}

private:
    hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override;

    task<std::size_t>
    drain_more_task(
        value_chunk_view chunks,
        std::size_t splat);

    static std::span<const std::byte> first_nonempty(
        value_chunk_view chunks,
        std::size_t splat) noexcept;

    io_handle fd_ = invalid_io_handle;
};

#if !defined(_WIN32)
/// An @ref fd_sink over standard output, with an owned buffer.
fd_sink standard_output(std::size_t buffer_size = 4096);

/// Same as @ref standard_output.
fd_sink standard_output_sink(std::size_t buffer_size = 4096);
#endif

/// Buffered byte sink over a connected socket.
///
/// Like @ref fd_sink, but each drain is one `op::send_some` wish with the
/// given `send(2)` flags, and the sink counts bytes sent. The socket is
/// borrowed and never closed by the sink.
class socket_sink final : public bytesink
{
public:
    explicit socket_sink(
        socket_handle fd,
        std::span<std::byte> buffer,
        int flags = 0)
        : bytesink(buffer)
        , fd_(fd)
        , flags_(flags)
    {}

    explicit socket_sink(
        socket_handle fd,
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
        std::size_t splat) override;

    task<std::size_t>
    drain_more_task(
        value_chunk_view chunks,
        std::size_t splat);

    static std::span<const std::byte> first_nonempty(
        value_chunk_view chunks,
        std::size_t splat) noexcept;

    socket_handle fd_ = invalid_socket_handle;
    int flags_ = 0;
    std::size_t sent_ = 0;
};

/// Write text, bytes, or a range of chunks, then flush.
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
// The feed cold path is `stream_more()`, the Zig-shaped verb:
// push up to `limit` bytes into a `bytesink`. Like Zig, an implementation may
// also choose to put bytes in its own reader buffer and return zero streamed
// bytes; the next public `stream()`/`take()` call will consume those buffered
// bytes through the hot path. Pull-shaped refill is derived by pointing a fixed
// sink at the feed's unused capacity, so source bytes still land in the
// feed's own buffer before borrowed-span APIs (`peek`/`take`) expose them. This gives us
// the structure of Zig's "stream into a sink, and refill is just streaming into
// my own buffer" without yet caring about fd-to-fd sendfile-style optimization.
//
// The remaining divergence is that most nxt reader verbs still require at
// least one value of storage. Zig can express "fill my Reader.buffer" through
// `readVec`'s special empty-slice convention; our concrete refill has nowhere
// to put source bytes unless the reader owns or borrows a real span. With zero
// capacity `take()` still works through a temporary cell, and `stream()` /
// `read()` can bypass the buffer, but the borrowing verbs (`peek`, `take(n)`,
// ...) throw.
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
// The wider Zig vocabulary is structural: `stream()` and `read_vec()` use
// buffered bytes when they have them and otherwise call `stream_more()`
// directly (`read_vec()` through a fixed sink over the caller's span).
// `discard()` uses buffered bytes when it has them and otherwise calls
// overridable `discard_more()`. Refill is `stream_more()` into a fixed sink
// over this reader's own unused capacity. There is no separate `readVec` or
// `rebase` virtual.
//
// On the sink side the staging buffer is a ring (`ring_region`) with a read
// cursor as well as a write cursor, so partially drained buffered output is
// represented honestly. That makes Zig-style `rebase(preserve, capacity)`
// direct: drain only the non-preserved prefix and keep the recent suffix
// staged. It does not compact, so it can fail when free space is split
// around the preserved values. Zig's writer source has a TODO wishing for
// this because its default rebase logic temporarily hides preserved bytes by
// mutating `end`.
//
// --- What we still owe to fully adopt the paradigm --------------------------
//
// TODO(zig-stream): teach concrete fd/socket feeds and sinks about each
//   other so `stream_more()` can eventually use sendfile/readv/writev-shaped
//   paths. Today it has the right API shape but still moves ordinary spans.
// TODO(zig-readvec): teach fd/socket/task-backed sources real scatter reads.
//   `read_vec()` still streams into only the first non-empty destination,
//   matching Zig's simple default; a virtual slot for it does not exist yet.
// TODO(zig-discard): optimized `discard_more(limit)` overrides so protocols can
//   skip bytes (chunked trailers, body skip-to-end) without buffering and
//   copying them through a sink-shaped shim.
// TODO(zig-rebase): add a virtual rebase slot for a ring- or mmap-backed
//   reader that can make room differently from memmove.
// TODO(eager-wand): push the synchronous-completion idea of `stream_more()` down
//   to the wish layer -- an honest `urge::await_ready()` plus a sync path in
//   `wand::prepare` -- so a warm `read_some` on the fd also skips the
//   round-trip. At that point the buffered feed can BE a wand and `hope`
//   dissolves into a single "maybe already here, else suspends" awaitable
//   shared by wishes and readers alike. That is the endgame this whole family
//   is shaped toward.

/// Byte feed: @ref feed of `std::byte`, the reader side of byte I/O.
///
/// Adds the span-borrowing verbs (`take(n)`, `peek_span`, `take_some`,
/// `take_until`, `read`). Concrete sources include @ref fd_source,
/// @ref socket_source, @ref byte_span_feed, @ref task_bytefeed, and the
/// decompressors in compression.hpp.
using bytefeed = feed<std::byte>;

/// Causal frame facade over a `feed<Stock>`.
///
/// `reel` is the feed-moving half of @ref rfc_reels_reel
/// "RFC 0001: Reels / Reel". It owns no source stock and caches no frame marks:
/// `visible()` returns a fresh @ref chop_view over `source_.buffered()`, while
/// `peek()` only fills the underlying feed until enough complete chops are
/// visible. Frame projections stay borrowed from the source feed and are invalid
/// after the next reel/feed operation that may mutate its buffer.
template<
    typename Stock,
    typename Frame,
    chop_scanner<Stock, Frame> Scanner =
        static_chop_scanner<Stock, Frame>>
class reel
{
public:
    using stock_type = Stock;
    using frame_type = Frame;
    using scanner_type = Scanner;
    using source_type = feed<Stock>;
    using view_type = chop_view<Stock, Frame, Scanner>;

    explicit reel(source_type & source, Scanner scanner = {})
        : source_(source)
        , scanner_(std::move(scanner))
    {}

    [[nodiscard]] view_type visible() const
    {
        return chop<Stock, Frame>(source_.buffered(), scanner_);
    }

    /// Fill the source until at least `minimum_count` complete frames are
    /// visible, or it ends; then return the visible frames.
    ///
    /// The end of the source is not an error here: the view may hold fewer
    /// frames than asked for.
    hope<view_type> peek(std::size_t minimum_count = 1)
    {
        try {
            while (!visible().has_at_least(minimum_count)) {
                auto target = next_fill_target();
                auto fill = source_.fill(target);
                if (!fill.is_ready())
                    return peek_slow(minimum_count, std::move(fill));
            }
        } catch (const value_end_of_stream &) {
        }

        return hope<view_type>::ready(visible());
    }

    /// Consume `extent` source values. Throws `value_end_of_stream` if a
    /// discard step makes no progress, as at the end of the source.
    hope<void> discard_prefix(std::size_t extent)
    {
        if (extent == 0)
            return hope<void>::ready();

        auto discarded = source_.discard(extent);
        if (discarded.is_ready()) {
            auto result = discarded.take_ready();
            auto n = value_count(result);
            if (n > extent)
                throw buffer_error{"frame discard overreported extent"};
            if (n == extent)
                return hope<void>::ready();
            if (n == 0 && is_eof(result))
                throw value_end_of_stream{
                    "unexpected end of frame input",
                };
            if (n == 0)
                return discard_prefix_slow(extent);
            return discard_prefix_slow(extent - n);
        }

        return discard_prefix_slow(std::move(discarded), extent);
    }

    /// Consume the source values occupied by `frame`.
    hope<void> discard(frame_chop<Frame> const & frame)
    {
        return discard_prefix(frame.extent);
    }

private:
    task<view_type> peek_slow(
        std::size_t minimum_count,
        hope<void> first_fill)
    {
        try {
            co_await std::move(first_fill);
            while (!visible().has_at_least(minimum_count))
                co_await source_.fill(next_fill_target());
        } catch (const value_end_of_stream &) {
        }
        co_return visible();
    }

    task<void> discard_prefix_slow(std::size_t remaining)
    {
        while (remaining != 0) {
            auto discarded = co_await source_.discard(remaining);
            auto n = value_count(discarded);
            if (n > remaining)
                throw buffer_error{"frame discard overreported extent"};
            if (n == 0 && is_eof(discarded))
                throw value_end_of_stream{
                    "unexpected end of frame input",
                };
            if (n == 0)
                continue;
            remaining -= n;
        }
    }

    task<void> discard_prefix_slow(
        hope<fare_t> first_discard,
        std::size_t remaining)
    {
        auto discarded = co_await std::move(first_discard);
        auto n = value_count(discarded);
        if (n > remaining)
            throw buffer_error{"frame discard overreported extent"};
        if (n == 0 && is_eof(discarded))
            throw value_end_of_stream{
                "unexpected end of frame input",
            };
        if (n == 0) {
            co_await discard_prefix_slow(remaining);
            co_return;
        }
        co_await discard_prefix_slow(remaining - n);
    }

    [[nodiscard]] std::size_t next_fill_target() const
    {
        auto stock = source_.buffered();
        auto offset = std::size_t{0};

        while (true) {
            auto suffix = stock.subspan(offset);
            if (suffix.empty())
                return offset + 1;

            auto scan = std::invoke(scanner_, suffix);
            if (auto * frame = std::get_if<frame_chop<Frame>>(&scan)) {
                if (frame->extent == 0)
                    throw buffer_error{"chop scanner returned zero extent"};
                if (frame->extent > suffix.size())
                    throw buffer_error{
                        "chop scanner overreported extent",
                    };
                offset += frame->extent;
                continue;
            }

            auto need = std::get<chop_need_more>(scan);
            auto requested = offset + need.minimum_buffered;
            return std::max(requested, stock.size() + 1);
        }
    }

    source_type & source_;
    Scanner scanner_;
};

/// Byte feed backed by a callable returning `task<fare_t>` or
/// `task<std::size_t>`. Count-only reads treat zero bytes as EOF. See
/// @ref nxtrt::taskfeed "taskfeed".
template<byte_read_task Read>
class task_bytefeed final : public taskfeed<std::byte, Read>
{
    using base = taskfeed<std::byte, Read>;

public:
    using base::base;
};

template<typename Read, std::size_t Extent>
task_bytefeed(Read, std::span<std::byte, Extent>) -> task_bytefeed<Read>;

template<typename Read>
task_bytefeed(Read, value_storage_ref<std::byte>) -> task_bytefeed<Read>;

/// In-memory byte feed over a range of byte-like chunks.
///
/// Chunks may be byte spans or text views; text chunks are treated as their
/// underlying bytes. The range is wrapped with `std::views::all`, so an
/// lvalue range is borrowed, and the bytes each chunk refers to must outlive
/// the feed. Ready writes keep `stream_more()` from suspending, so reads
/// never need a deck turn. The range is traversed once. The default buffer
/// is 4096 bytes.
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

/// Buffered byte feed over a file descriptor.
///
/// Each refill is one `op::read_some` wish, retried on `EINTR`, straight into
/// the destination's free space. A zero-byte read is EOF. Other read errors
/// propagate as exceptions. The fd is borrowed and never closed by the feed.
/// The default buffer is 4096 bytes.
class fd_source final : public detail::taskfeed_base<std::byte, fd_source>
{
    using base = detail::taskfeed_base<std::byte, fd_source>;

public:
    explicit fd_source(io_handle fd, std::span<std::byte> buffer)
        : base(buffer)
        , fd_(fd)
    {}

    explicit fd_source(io_handle fd, std::size_t buffer_size = 4096)
        : base(buffer_size)
        , fd_(fd)
    {}

    task<std::size_t> read_into(junk<std::byte> dst);

private:
    friend base;

    io_handle fd_ = invalid_io_handle;
};

/// Buffered byte feed over a connected socket.
///
/// Like @ref fd_source, but each refill is one `op::recv_some` wish with the
/// given `recv(2)` flags, and the feed counts bytes received. A zero-byte
/// receive (orderly shutdown) is EOF.
class socket_source final : public detail::taskfeed_base<std::byte, socket_source>
{
    using base = detail::taskfeed_base<std::byte, socket_source>;

public:
    explicit socket_source(
        socket_handle fd,
        std::span<std::byte> buffer,
        int flags = 0)
        : base(buffer)
        , fd_(fd)
        , flags_(flags)
    {}

    explicit socket_source(
        socket_handle fd,
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

    task<std::size_t> read_into(junk<std::byte> dst);

private:
    friend base;

    socket_handle fd_ = invalid_socket_handle;
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
task<std::size_t> stream_all(
    bytefeed & reader,
    bytesink & writer);

} // namespace nxtrt
