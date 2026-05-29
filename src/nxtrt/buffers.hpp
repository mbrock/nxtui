#pragma once

#include "nxtrt/task.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstring>
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

inline std::span<const std::byte> first_nonempty(
    std::span<const std::span<const std::byte>> chunks) noexcept
{
    for (auto chunk : chunks) {
        if (!chunk.empty())
            return chunk;
    }
    return {};
}

} // namespace detail

/// Buffered asynchronous writer base.
///
/// The hot-path methods are non-virtual and copy into `buffer_` when there is
/// capacity. An empty buffer is valid and means unbuffered writes: non-empty
/// writes always go through the slow path.
///
/// Pending output is `buffer_[seek_..end_]`; bytes before `seek_` have already
/// been consumed by the sink. Derived writers implement `drain_more()`, the
/// slow-path operation that drains a vector of byte spans to the concrete
/// backend.
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

    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.subspan(seek_, end_ - seek_);
    }

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_ - seek_;
    }

    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    void advance(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw buffer_error{"writer advanced past buffer capacity"};
        end_ += n;
    }

    hope<void> flush()
    {
        if (buffered_size() == 0) {
            reset_if_empty();
            return hope<void>::ready();
        }
        return flush_slow();
    }

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

    task<void> write(std::string text)
    {
        co_await write(as_bytes(std::string_view{text}));
    }

    hope<void> write(std::string_view text)
    {
        return write(as_bytes(text));
    }

    template<detail::byte_writer_chunk_range Chunks>
    task<void> write(Chunks && chunks)
    {
        for (auto && chunk : chunks)
            co_await write(std::forward<decltype(chunk)>(chunk));
    }

    template<typename Chunks>
        requires detail::byte_writer_chunk<Chunks>
            || detail::byte_writer_chunk_range<Chunks>
    task<void> write_all(Chunks && chunks)
    {
        co_await write(std::forward<Chunks>(chunks));
        co_await flush();
    }

protected:
    virtual hope<std::size_t> drain_more(
        std::span<const std::span<const std::byte>> chunks) = 0;

private:
    void append_to_buffer(std::span<const std::byte> bytes)
    {
        std::memcpy(buffer_.data() + end_, bytes.data(), bytes.size());
        advance(bytes.size());
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

    task<void> flush_slow()
    {
        while (buffered_size() != 0) {
            auto pending = buffered();
            auto chunks = std::array{pending};
            auto written = co_await drain_more(std::span{chunks});
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
            auto written = co_await drain_more(std::span{chunks});
            require_progress(written, count);

            auto from_pending = std::min(written, pending.size());
            consume_buffered(from_pending);
            written -= from_pending;
            if (written > remaining.size())
                throw buffer_error{"writer overreported written bytes"};
            remaining = remaining.subspan(written);
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
    drain_more(std::span<const std::span<const std::byte>>) override
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
    drain_more(std::span<const std::span<const std::byte>> chunks) override
    {
        return hope<std::size_t>::ready(detail::byte_size(chunks));
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
    drain_more(std::span<const std::span<const std::byte>> chunks) override
    {
        return drain_more_task(chunks);
    }

    task<std::size_t>
    drain_more_task(std::span<const std::span<const std::byte>> chunks)
    {
        auto src = detail::first_nonempty(chunks);
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

private:
    hope<std::size_t>
    drain_more(std::span<const std::span<const std::byte>> chunks) override
    {
        return drain_more_task(chunks);
    }

    task<std::size_t>
    drain_more_task(std::span<const std::span<const std::byte>> chunks)
    {
        auto src = detail::first_nonempty(chunks);
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
    drain_more(std::span<const std::span<const std::byte>> chunks) override
    {
        return drain_more_task(chunks);
    }

    task<std::size_t>
    drain_more_task(std::span<const std::span<const std::byte>> chunks)
    {
        auto total = detail::byte_size(chunks);
        for (auto chunk : chunks)
            co_await inner_.write(chunk);
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
// push up to `limit` bytes into a `byte_writer`. Pull-shaped refill is derived
// by pointing a fixed writer at the reader's unused capacity, so source bytes
// still land in `buffer_` before borrowed-span APIs (`peek`/`take`) expose
// them. This gives us the structure of Zig's "stream into a writer, and refill
// is just streaming into my own buffer" without yet caring about fd-to-fd
// sendfile-style optimization.
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
// The wider Zig vocabulary is now structural: `stream()` uses buffered bytes
// when it has them and otherwise calls `stream_more()` directly; `discard()`
// uses buffered bytes when it has them and otherwise streams into a discarding
// writer. Refill is just `stream_more(fixed_writer{unused_capacity()}, limit)`.
//
// On the writer side we intentionally one-up Zig a little: `byte_writer` has a
// reader-like `seek_` as well as `end_`, so partially drained buffered output
// is represented honestly as `buffer_[seek_..end_]`. Zig's writer source has a
// TODO wishing for this because its default rebase logic temporarily hides
// preserved bytes by mutating `end`.
//
// --- What we still owe to fully adopt the paradigm --------------------------
//
// TODO(zig-stream): teach concrete fd/socket readers and writers about each
//   other so `stream_more()` can eventually use sendfile/readv/writev-shaped
//   paths. Today it has the right API shape but still moves ordinary spans.
// TODO(zig-readvec): a vectored refill/read API that can fill caller-provided
//   scatter buffers directly (not just `buffer_`), to avoid a copy on large
//   `take(n)` where the caller already owns the destination.
// TODO(zig-discard): optimized `discard(limit)` overrides so protocols can skip
//   bytes (chunked trailers, body skip-to-end) without buffering and copying
//   them through a writer-shaped shim.
// TODO(zig-rebase): make `rebase` virtual (Zig keeps it in the vtable with a
//   memmove default) so a ring- or mmap-backed reader can make room differently.
// TODO(eager-wand): push the synchronous-completion idea of `stream_more()` down
//   to the wish layer -- an honest `waiter::await_ready()` plus a sync path in
//   `wand::prepare` -- so a warm `read_some` on the fd also skips the
//   round-trip. At that point the buffered reader can BE a wand and `hope`
//   dissolves into a single "maybe already here, else suspends" awaitable
//   shared by wishes and readers alike. That is the endgame this whole family
//   is shaped toward.

/// Buffered asynchronous reader base.
///
/// The hot-path methods are non-virtual and operate directly on `buffer_`,
/// `seek_`, and `end_`, returning `hope<...>` so the buffered case never
/// suspends. Derived readers implement only `stream_more()`, the cold path that
/// pushes source bytes into a writer. Refill is derived by streaming into a
/// fixed writer over `unused_capacity()`, which then advances `end_` by the
/// returned byte count.
class byte_reader
{
public:
    explicit byte_reader(std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , buffer_(owned_buffer_)
    {
        if (owned_buffer_.empty())
            throw buffer_error{"reader buffer is empty"};
    }

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

    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.subspan(seek_, end_ - seek_);
    }

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_ - seek_;
    }

    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    void rebase(std::size_t capacity)
    {
        if (capacity > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffer_.size() - seek_ >= capacity)
            return;

        auto pending = buffered_size();
        std::memmove(buffer_.data(), buffer_.data() + seek_, pending);
        seek_ = 0;
        end_ = pending;
    }

    hope<void> fill(std::size_t n)
    {
        if (n > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n)
            return hope<void>::ready();

        return fill_slow(n);
    }

    hope<read_result> fill_more()
    {
        if (buffered_size() == buffer_.size())
            throw buffer_error{"reader buffer is full"};
        rebase(buffered_size() + 1);
        return fill_more_without_rebase();
    }

    hope<std::span<const std::byte>> peek(std::size_t n)
    {
        if (n > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n)
            return hope<std::span<const std::byte>>::ready(
                buffered().first(n));
        return peek_slow(n);
    }

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

    void toss(std::size_t n)
    {
        if (n > buffered_size())
            throw buffer_error{"reader consumed past buffered input"};
        seek_ += n;
    }

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

    hope<std::string_view> take_string_view(std::size_t n)
    {
        auto bytes = take(n);
        if (bytes.is_ready())
            return hope<std::string_view>::ready(
                as_string_view(bytes.take_ready()));
        return take_string_view_slow(std::move(bytes));
    }

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

    hope<std::span<const std::byte>>
    take_until(std::string_view delimiter)
    {
        return take_until(as_bytes(delimiter));
    }

    /// Transfer one available chunk to `writer`, up to `limit` bytes.
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
    [[nodiscard]] std::span<std::byte> buffer_storage() noexcept
    {
        return buffer_;
    }

    virtual hope<read_result> stream_more(
        byte_writer & writer,
        std::size_t limit) = 0;

private:
    void append_read_bytes(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw buffer_error{"source overfilled read buffer"};
        end_ += n;
    }

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

    hope<read_result> discard_more(std::size_t limit)
    {
        auto writer = discarding_byte_writer{buffer_};
        auto result = stream_more(writer, limit);
        if (result.is_ready())
            return hope<read_result>::ready(result.take_ready());
        return discard_more_slow(limit);
    }

    hope<read_result> read_more()
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw buffer_error{"reader buffer is full"};

        auto writer = fixed_byte_writer{dst};
        auto result = stream_more(writer, dst.size());
        if (result.is_ready())
            return hope<read_result>::ready(
                finish_stream_read(writer, result.take_ready()));
        return read_more_slow(dst.size());
    }

    read_result finish_stream_read(
        fixed_byte_writer & writer,
        read_result result)
    {
        auto written = writer.buffered_size();
        if (written != result.bytes)
            throw buffer_error{"reader stream byte count mismatch"};
        append_read_bytes(written);
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
        auto writer = fixed_byte_writer{unused_capacity().first(limit)};
        auto result = co_await stream_more(writer, limit);
        co_return finish_stream_read(writer, result);
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

        auto storage = std::vector<std::byte>(std::min(limit, std::size_t{4096}));
        auto result = co_await std::invoke(read_, std::span{storage});
        auto out = normalize_result(result);
        if (out.bytes > storage.size())
            throw buffer_error{"source overfilled temporary buffer"};
        if (out.bytes != 0)
            co_await writer.write(std::span{storage}.first(out.bytes));
        co_return out;
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
            auto storage =
                std::vector<std::byte>(std::min(limit, std::size_t{4096}));
            auto n = co_await read_some(std::span{storage});
            if (n != 0)
                co_await writer.write(std::span{storage}.first(n));
            co_return read_result{.bytes = n, .eof = n == 0};
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
            auto storage =
                std::vector<std::byte>(std::min(limit, std::size_t{4096}));
            auto n = co_await recv_some(std::span{storage});
            if (n != 0)
                co_await writer.write(std::span{storage}.first(n));
            co_return read_result{.bytes = n, .eof = n == 0};
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

} // namespace nxtrt
