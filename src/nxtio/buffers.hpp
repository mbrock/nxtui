#pragma once

#include <nxtio/async.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace nxt::io {

struct buffer_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct end_of_stream : buffer_error
{
    using buffer_error::buffer_error;
};

struct operation_cancelled : buffer_error
{
    using buffer_error::buffer_error;
};

inline std::string_view
as_string_view(std::span<const std::byte> bytes) noexcept;

class string_source
{
public:
    // Borrowed sequence of string chunks, exposed as one contiguous byte
    // source.  The caller owns the span and the string_view storage.
    explicit string_source(std::span<const std::string_view> chunks)
        : chunks_(chunks)
    {
    }

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

class string_sink
{
public:
    nxt::task<> write_all(std::span<const std::byte> bytes)
    {
        text_ += as_string_view(bytes);
        co_return;
    }

    nxt::task<> write_all(std::string_view text)
    {
        text_ += text;
        co_return;
    }

    [[nodiscard]] const std::string & text() const noexcept
    {
        return text_;
    }

private:
    std::string text_;
};

class string_transport
{
public:
    // Small in-memory transport for tests, protocol fixtures, and replay
    // harnesses.  Incoming bytes are borrowed; outgoing bytes are collected.
    explicit string_transport(std::span<const std::string_view> chunks)
        : source_(chunks)
    {
    }

    nxt::task<std::size_t> read_some(std::span<std::byte> dst)
    {
        co_return co_await source_.read_some(dst);
    }

    nxt::task<> write_all(std::span<const std::byte> bytes)
    {
        co_await sink_.write_all(bytes);
    }

    nxt::task<> write_all(std::string_view text)
    {
        co_await sink_.write_all(text);
    }

    [[nodiscard]] const std::string & written() const noexcept
    {
        return sink_.text();
    }

private:
    string_source source_;
    string_sink sink_;
};

inline std::span<char> as_writable_chars(std::span<std::byte> bytes) noexcept
{
    return {
        reinterpret_cast<char *>(bytes.data()),
        bytes.size_bytes(),
    };
}

inline std::span<const char>
as_chars(std::span<const std::byte> bytes) noexcept
{
    return {
        reinterpret_cast<const char *>(bytes.data()),
        bytes.size_bytes(),
    };
}

inline std::string_view
as_string_view(std::span<const std::byte> bytes) noexcept
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

class byte_cursor
{
public:
    explicit byte_cursor(std::span<const std::byte> bytes)
        : rest_(bytes)
    {
    }

    explicit byte_cursor(std::string_view text)
        : rest_(as_bytes(text))
    {
    }

    [[nodiscard]] std::span<const std::byte> remaining() const noexcept
    {
        return rest_;
    }

    void toss(std::size_t n)
    {
        if (n > rest_.size())
            throw buffer_error{"cursor consumed past end of input"};
        rest_ = rest_.subspan(n);
    }

    std::span<const std::byte> take(std::size_t n)
    {
        if (n > rest_.size())
            throw buffer_error{"cursor consumed past end of input"};
        auto out = rest_.first(n);
        toss(n);
        return out;
    }

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

    std::span<const std::byte> take_until(std::string_view delimiter)
    {
        return take_until(as_bytes(delimiter));
    }

private:
    std::span<const std::byte> rest_;
};

template<typename Source>
class byte_reader
{
public:
    byte_reader(
        Source & source,
        std::span<std::byte> buffer,
        std::stop_token stop = {})
        : source_(&source)
        , buffer_(buffer)
        , stop_(std::move(stop))
    {
    }

    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.subspan(seek_, end_ - seek_);
    }

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_ - seek_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return buffer_.size();
    }

    [[nodiscard]] std::size_t seek() const noexcept
    {
        return seek_;
    }

    [[nodiscard]] std::size_t end() const noexcept
    {
        return end_;
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

    nxt::task<std::size_t> fill_more()
    {
        if (buffered_size() == buffer_.size())
            throw buffer_error{"reader buffer is full"};
        rebase(buffered_size() + 1);
        co_return co_await fill_more_without_rebase();
    }

    nxt::task<std::span<const std::byte>> peek(std::size_t n)
    {
        co_await fill(n);
        co_return buffered().first(n);
    }

    void toss(std::size_t n)
    {
        if (n > buffered_size())
            throw buffer_error{"reader consumed past buffered input"};
        seek_ += n;
    }

    nxt::task<std::span<const std::byte>> take(std::size_t n)
    {
        auto out = co_await peek(n);
        toss(n);
        co_return out;
    }

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

    nxt::task<std::span<const std::byte>>
    take_until(std::string_view delimiter)
    {
        co_return co_await take_until(as_bytes(delimiter));
    }

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

template<typename Sink>
class byte_writer
{
public:
    byte_writer(
        Sink & sink,
        std::span<std::byte> buffer,
        std::stop_token stop = {})
        : sink_(&sink)
        , buffer_(buffer)
        , stop_(std::move(stop))
    {
    }

    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.first(end_);
    }

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return buffer_.size();
    }

    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    nxt::task<std::span<std::byte>> writable(std::size_t n)
    {
        co_await ensure_unused_capacity(n);
        co_return unused_capacity().first(n);
    }

    void advance(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw buffer_error{"writer advanced past unused capacity"};
        end_ += n;
    }

    void undo(std::size_t n)
    {
        if (n > end_)
            throw buffer_error{"writer undo moved before buffer start"};
        end_ -= n;
    }

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

    nxt::task<> write_all(std::string_view text)
    {
        co_await write_all(as_bytes(text));
    }

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
