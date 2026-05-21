#pragma once

#include <nxtio/async.hpp>
#include <nxtio/stacktrace.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace nxt::io {

/// Base exception for buffered byte I/O helpers.
struct buffer_error : runtime_error
{
    using runtime_error::runtime_error;
};

/// Raised when a reader needs more bytes but the source ended.
struct end_of_stream : buffer_error
{
    using buffer_error::buffer_error;
};

/// Raised when a stop token cancels a buffered operation.
struct operation_cancelled : buffer_error
{
    using buffer_error::buffer_error;
};

/// View a byte span as immutable character data.
inline std::string_view
as_string_view(std::span<const std::byte> bytes) noexcept;

/// Borrowed in-memory source exposed through `read_some`.
class string_source
{
public:
    /// Borrow a sequence of string chunks as one byte stream.
    explicit string_source(std::span<const std::string_view> chunks)
        : chunks_(chunks)
    {
    }

    /// Read bytes from the current chunk sequence into `dst`.
    nxt::task<std::size_t> read_some(std::span<std::byte> dst)
    {
        auto written = std::size_t{0};
        while (written < dst.size() && chunk_ < chunks_.size()) {
            auto chunk = chunks_[chunk_];
            auto rest = chunk.substr(offset_);
            auto n = std::min(dst.size() - written, rest.size());
            std::ranges::copy(
                std::as_bytes(std::span{rest}).first(n),
                dst.begin() + static_cast<std::ptrdiff_t>(written));

            written += n;
            offset_ += n;
            if (offset_ == chunk.size()) {
                ++chunk_;
                offset_ = 0;
            }
        }

        co_return written;
    }

private:
    std::span<const std::string_view> chunks_;
    std::size_t chunk_ = 0;
    std::size_t offset_ = 0;
};

/// In-memory sink that appends all writes to a string.
class string_sink
{
public:
    /// Append raw bytes to the sink.
    nxt::task<> write_all(std::span<const std::byte> bytes)
    {
        text_ += as_string_view(bytes);
        co_return;
    }

    /// Append text bytes to the sink.
    nxt::task<> write_all(std::string_view text)
    {
        text_ += text;
        co_return;
    }

    /// Return all bytes written so far.
    [[nodiscard]] const std::string & text() const noexcept
    {
        return text_;
    }

private:
    std::string text_;
};

/// In-memory transport for tests, fixtures, and replay harnesses.
class string_transport
{
public:
    /// Borrow incoming chunks; outgoing bytes are collected.
    explicit string_transport(std::span<const std::string_view> chunks)
        : source_(chunks)
    {
    }

    /// Read from the borrowed input source.
    nxt::task<std::size_t> read_some(std::span<std::byte> dst)
    {
        co_return co_await source_.read_some(dst);
    }

    /// Write raw bytes to the collected output.
    nxt::task<> write_all(std::span<const std::byte> bytes)
    {
        co_await sink_.write_all(bytes);
    }

    /// Write text bytes to the collected output.
    nxt::task<> write_all(std::string_view text)
    {
        co_await sink_.write_all(text);
    }

    /// Return all bytes written through this transport.
    [[nodiscard]] const std::string & written() const noexcept
    {
        return sink_.text();
    }

private:
    string_source source_;
    string_sink sink_;
};

/// Reinterpret writable bytes as writable chars.
inline std::span<char> as_writable_chars(std::span<std::byte> bytes) noexcept
{
    return {
        reinterpret_cast<char *>(bytes.data()),
        bytes.size_bytes(),
    };
}

/// Reinterpret immutable bytes as chars.
inline std::span<const char>
as_chars(std::span<const std::byte> bytes) noexcept
{
    return {
        reinterpret_cast<const char *>(bytes.data()),
        bytes.size_bytes(),
    };
}

/// Reinterpret immutable bytes as a string view.
inline std::string_view
as_string_view(std::span<const std::byte> bytes) noexcept
{
    return {
        reinterpret_cast<const char *>(bytes.data()),
        bytes.size_bytes(),
    };
}

/// Reinterpret a string view as immutable bytes.
inline std::span<const std::byte> as_bytes(std::string_view text) noexcept
{
    return std::as_bytes(std::span{text});
}

namespace detail {

inline void check_cancelled(std::stop_token stop)
{
    if (stop.stop_requested())
        throw operation_cancelled{"operation cancelled"};
}

template<typename Source>
nxt::task<std::size_t>
read_some_bytes(Source & source, std::span<std::byte> dst)
{
    if constexpr (requires { source.read_some(dst); }) {
        co_return co_await source.read_some(dst);
    } else {
        co_return co_await source.read_some(as_writable_chars(dst));
    }
}

template<typename Source>
nxt::task<std::size_t>
read_some_bytes(
    Source & source,
    std::span<std::byte> dst,
    std::stop_token stop)
{
    if constexpr (requires { source.read_some(dst, stop); }) {
        co_return co_await source.read_some(dst, std::move(stop));
    } else if constexpr (
        requires { source.read_some(as_writable_chars(dst), stop); }) {
        co_return co_await source.read_some(
            as_writable_chars(dst),
            std::move(stop));
    } else {
        check_cancelled(stop);
        co_return co_await read_some_bytes(source, dst);
    }
}

template<typename Sink>
nxt::task<> write_all_bytes(Sink & sink, std::span<const std::byte> bytes)
{
    if constexpr (requires { sink.write_all(bytes); }) {
        co_await sink.write_all(bytes);
    } else {
        co_await sink.write_all(as_string_view(bytes));
    }
}

inline std::size_t find_bytes(
    std::span<const std::byte> haystack,
    std::span<const std::byte> needle)
{
    auto match = std::ranges::search(haystack, needle);
    return static_cast<std::size_t>(
        std::distance(haystack.begin(), match.begin()));
}

} // namespace detail

/// Synchronous cursor over an already-buffered byte span.
class byte_cursor
{
public:
    /// Create a cursor over bytes.
    explicit byte_cursor(std::span<const std::byte> bytes)
        : rest_(bytes)
    {
    }

    /// Create a cursor over text bytes.
    explicit byte_cursor(std::string_view text)
        : rest_(as_bytes(text))
    {
    }

    /// Remaining unread bytes.
    [[nodiscard]] std::span<const std::byte> remaining() const noexcept
    {
        return rest_;
    }

    /// Discard `n` bytes.
    void toss(std::size_t n)
    {
        if (n > rest_.size())
            throw buffer_error{"cursor consumed past end of input"};
        rest_ = rest_.subspan(n);
    }

    /// Consume and return exactly `n` bytes.
    std::span<const std::byte> take(std::size_t n)
    {
        if (n > rest_.size())
            throw buffer_error{"cursor consumed past end of input"};
        auto out = rest_.first(n);
        toss(n);
        return out;
    }

    /// Consume through `delimiter` and return bytes before it.
    std::span<const std::byte> take_until(std::span<const std::byte> delimiter)
    {
        if (delimiter.empty())
            throw buffer_error{"empty delimiter"};

        auto cut = detail::find_bytes(rest_, delimiter);
        if (cut == rest_.size())
            throw buffer_error{"delimiter was absent"};

        auto out = rest_.first(cut);
        toss(cut + delimiter.size());
        return out;
    }

    /// Consume through a string delimiter and return bytes before it.
    std::span<const std::byte> take_until(std::string_view delimiter)
    {
        return take_until(as_bytes(delimiter));
    }

private:
    std::span<const std::byte> rest_;
};

/// Buffered asynchronous reader over any source with a `read_some` method.
template<typename Source>
class byte_reader
{
public:
    /// Use caller-owned storage as the read-ahead buffer.
    byte_reader(
        Source & source,
        std::span<std::byte> buffer,
        std::stop_token stop = {})
        : source_(&source)
        , buffer_(buffer)
        , stop_(std::move(stop))
    {
    }

    /// Bytes currently buffered and not yet consumed.
    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.subspan(seek_, end_ - seek_);
    }

    /// Number of buffered bytes.
    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_ - seek_;
    }

    /// Total buffer capacity.
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return buffer_.size();
    }

    /// Start offset of unread bytes in the buffer.
    [[nodiscard]] std::size_t seek() const noexcept
    {
        return seek_;
    }

    /// End offset of written bytes in the buffer.
    [[nodiscard]] std::size_t end() const noexcept
    {
        return end_;
    }

    /// Unused buffer storage after the current end offset.
    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    /// Move unread bytes to the front when needed to expose capacity.
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

    /// Ensure at least `n` bytes are buffered.
    nxt::task<> fill(std::size_t n)
    {
        if (n > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n)
            co_return;

        rebase(n);
        while (buffered_size() < n) {
            auto read = co_await fill_more_without_rebase();
            if (read == 0)
                throw end_of_stream{"unexpected end of input"};
        }
    }

    /// Read more bytes from the source and return the count read.
    nxt::task<std::size_t> fill_more()
    {
        if (buffered_size() == buffer_.size())
            throw buffer_error{"reader buffer is full"};
        rebase(buffered_size() + 1);
        co_return co_await fill_more_without_rebase();
    }

    /// Return the next `n` bytes without consuming them.
    nxt::task<std::span<const std::byte>> peek(std::size_t n)
    {
        co_await fill(n);
        co_return buffered().first(n);
    }

    /// Discard buffered bytes.
    void toss(std::size_t n)
    {
        if (n > buffered_size())
            throw buffer_error{"reader consumed past buffered input"};
        seek_ += n;
    }

    /// Consume and return exactly `n` bytes.
    nxt::task<std::span<const std::byte>> take(std::size_t n)
    {
        auto out = co_await peek(n);
        toss(n);
        co_return out;
    }

    /// Consume through `delimiter` and return bytes before it.
    nxt::task<std::span<const std::byte>>
    take_until(std::span<const std::byte> delimiter)
    {
        if (delimiter.empty())
            throw buffer_error{"empty delimiter"};

        while (true) {
            auto available = buffered();
            auto cut = detail::find_bytes(available, delimiter);
            if (cut < available.size()) {
                auto out = available.first(cut);
                seek_ += cut + delimiter.size();
                co_return out;
            }

            if (buffered_size() == buffer_.size())
                throw buffer_error{"reader buffer filled before delimiter"};

            auto read = co_await fill_more();
            if (read == 0)
                throw end_of_stream{"unexpected end of input"};
        }
    }

    /// Consume through a string delimiter and return bytes before it.
    nxt::task<std::span<const std::byte>>
    take_until(std::string_view delimiter)
    {
        co_return co_await take_until(as_bytes(delimiter));
    }

    /// Stream exactly `n` bytes to a writer without materializing a string.
    template<typename Writer>
    nxt::task<> stream_exact(Writer & writer, std::size_t n)
    {
        auto remaining = n;
        while (remaining > 0) {
            auto available = buffered();
            if (!available.empty()) {
                auto count = std::min(available.size(), remaining);
                co_await writer.write_all(available.first(count));
                toss(count);
                remaining -= count;
                continue;
            }

            auto read = co_await fill_more();
            if (read == 0)
                throw end_of_stream{"unexpected end of input"};
        }
    }

private:
    nxt::task<std::size_t> fill_more_without_rebase()
    {
        detail::check_cancelled(stop_);
        auto dst = unused_capacity();
        if (dst.empty())
            throw buffer_error{"reader buffer is full"};

        auto n = co_await detail::read_some_bytes(*source_, dst, stop_);
        if (n > dst.size())
            throw buffer_error{"source overfilled read buffer"};
        end_ += n;
        co_return n;
    }

    Source * source_;
    std::span<std::byte> buffer_;
    std::size_t seek_ = 0;
    std::size_t end_ = 0;
    std::stop_token stop_;
};

/// Buffered asynchronous writer over any sink with a `write_all` method.
template<typename Sink>
class byte_writer
{
public:
    /// Use caller-owned storage as the write buffer.
    byte_writer(
        Sink & sink,
        std::span<std::byte> buffer,
        std::stop_token stop = {})
        : sink_(&sink)
        , buffer_(buffer)
        , stop_(std::move(stop))
    {
    }

    /// Bytes currently staged for writing.
    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.first(end_);
    }

    /// Number of staged bytes.
    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_;
    }

    /// Total buffer capacity.
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return buffer_.size();
    }

    /// Remaining writable capacity.
    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    /// Reserve `n` writable bytes, flushing first if needed.
    nxt::task<std::span<std::byte>> writable(std::size_t n)
    {
        co_await ensure_unused_capacity(n);
        co_return unused_capacity().first(n);
    }

    /// Mark `n` reserved bytes as written.
    void advance(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw buffer_error{"writer advanced past unused capacity"};
        end_ += n;
    }

    /// Remove `n` bytes from the staged output.
    void undo(std::size_t n)
    {
        if (n > end_)
            throw buffer_error{"writer undo moved before buffer start"};
        end_ -= n;
    }

    /// Ensure at least `n` bytes of unused capacity.
    nxt::task<> ensure_unused_capacity(std::size_t n)
    {
        if (n > buffer_.size())
            throw buffer_error{"writer buffer is too small"};
        if (unused_capacity().size() >= n)
            co_return;

        co_await flush();
        if (unused_capacity().size() < n)
            throw buffer_error{"writer could not make requested capacity"};
    }

    /// Write all bytes, buffering small chunks and bypassing for large chunks.
    nxt::task<> write_all(std::span<const std::byte> bytes)
    {
        auto rest = bytes;
        while (!rest.empty()) {
            auto dst = unused_capacity();
            if (dst.size() >= rest.size()) {
                std::memcpy(dst.data(), rest.data(), rest.size());
                end_ += rest.size();
                co_return;
            }

            if (!dst.empty()) {
                std::memcpy(dst.data(), rest.data(), dst.size());
                end_ += dst.size();
                rest = rest.subspan(dst.size());
            }

            co_await flush();

            if (rest.size() >= buffer_.size()) {
                detail::check_cancelled(stop_);
                co_await detail::write_all_bytes(*sink_, rest);
                co_return;
            }
        }
    }

    /// Write all text bytes.
    nxt::task<> write_all(std::string_view text)
    {
        co_await write_all(as_bytes(text));
    }

    /// Flush staged bytes to the sink.
    nxt::task<> flush()
    {
        if (end_ == 0)
            co_return;

        detail::check_cancelled(stop_);
        co_await detail::write_all_bytes(*sink_, buffered());
        end_ = 0;
    }

private:
    Sink * sink_;
    std::span<std::byte> buffer_;
    std::size_t end_ = 0;
    std::stop_token stop_;
};

} // namespace nxt::io
