#pragma once

#include "nxt/rt/task.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace nxt::rt {

struct buffer_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
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

/// Runtime-polymorphic byte source.
class byte_source
{
public:
    virtual ~byte_source() = default;

    virtual task<read_result> read_some(std::span<std::byte> dst) = 0;
};

/// Borrowed in-memory source exposed through the byte-source vtable.
class string_source final : public byte_source
{
public:
    explicit string_source(std::span<const std::string_view> chunks)
        : chunks_(chunks)
    {}

    task<read_result> read_some(std::span<std::byte> dst) override
    {
        auto written = std::size_t{0};
        while (written < dst.size() && chunk_ < chunks_.size()) {
            auto chunk = chunks_[chunk_];
            auto rest = chunk.substr(offset_);
            auto n = std::min(dst.size() - written, rest.size());
            std::memcpy(dst.data() + written, rest.data(), n);

            written += n;
            offset_ += n;
            if (offset_ == chunk.size()) {
                ++chunk_;
                offset_ = 0;
            }
        }

        co_return read_result{
            .bytes = written,
            .eof = chunk_ == chunks_.size(),
        };
    }

private:
    std::span<const std::string_view> chunks_;
    std::size_t chunk_ = 0;
    std::size_t offset_ = 0;
};

/// Byte source for a file descriptor.
class fd_source final : public byte_source
{
public:
    explicit fd_source(int fd) noexcept
        : fd_(fd)
    {}

    task<read_result> read_some(std::span<std::byte> dst) override
    {
        auto n = co_await read_some_wish{
            .fd = fd_,
            .buffer = dst,
            .offset = -1,
        };
        co_return read_result{
            .bytes = n,
            .eof = n == 0,
        };
    }

private:
    int fd_ = -1;
};

/// Buffered asynchronous reader over a byte source.
template<typename Source = byte_source>
class byte_reader
{
public:
    byte_reader(Source & source, std::span<std::byte> buffer)
        : source_(&source)
        , buffer_(buffer)
    {}

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

    task<> fill(std::size_t n)
    {
        if (n > buffer_.size())
            throw buffer_error{"reader buffer is too small"};
        if (buffered_size() >= n)
            co_return;

        rebase(n);
        while (buffered_size() < n) {
            auto read = co_await fill_more_without_rebase();
            if (read.eof && read.bytes == 0)
                throw end_of_stream{"unexpected end of input"};
        }
    }

    task<read_result> fill_more()
    {
        if (buffered_size() == buffer_.size())
            throw buffer_error{"reader buffer is full"};
        rebase(buffered_size() + 1);
        co_return co_await fill_more_without_rebase();
    }

    task<std::span<const std::byte>> peek(std::size_t n)
    {
        co_await fill(n);
        co_return buffered().first(n);
    }

    /// Consume and return the next borrowed chunk, up to `limit` bytes.
    ///
    /// The returned span may be empty. `std::nullopt` is the EOF signal.
    task<std::optional<std::span<const std::byte>>>
    take_some(std::size_t limit = std::numeric_limits<std::size_t>::max())
    {
        if (buffered_size() == 0) {
            auto read = co_await fill_more();
            if (read.eof && read.bytes == 0)
                co_return std::nullopt;
            if (read.bytes == 0)
                co_return buffered().first(0);
        }

        auto n = std::min(limit, buffered_size());
        auto out = buffered().first(n);
        toss(n);
        co_return out;
    }

    void toss(std::size_t n)
    {
        if (n > buffered_size())
            throw buffer_error{"reader consumed past buffered input"};
        seek_ += n;
    }

    task<std::span<const std::byte>> take(std::size_t n)
    {
        auto out = co_await peek(n);
        toss(n);
        co_return out;
    }

    task<std::span<const std::byte>>
    take_until(std::span<const std::byte> delimiter)
    {
        if (delimiter.empty())
            throw buffer_error{"empty delimiter"};

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

    task<std::span<const std::byte>>
    take_until(std::string_view delimiter)
    {
        co_return co_await take_until(as_bytes(delimiter));
    }

private:
    task<read_result> fill_more_without_rebase()
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw buffer_error{"reader buffer is full"};

        auto read = co_await source_->read_some(dst);
        if (read.bytes > dst.size())
            throw buffer_error{"source overfilled read buffer"};
        end_ += read.bytes;
        co_return read;
    }

    Source * source_;
    std::span<std::byte> buffer_;
    std::size_t seek_ = 0;
    std::size_t end_ = 0;
};

/// Repeatedly fill the same caller-owned buffer and visit each chunk.
///
/// `visitor` is called synchronously with the bytes read before the next read
/// is posted. The chunk span is only valid until the next loop iteration.
template<typename Visitor>
task<std::size_t> for_each_chunk(
    byte_source & source,
    std::span<std::byte> buffer,
    Visitor visitor)
{
    auto total = std::size_t{0};
    while (true) {
        auto read = co_await source.read_some(buffer);
        if (read.eof && read.bytes == 0)
            co_return total;

        auto chunk = std::span<const std::byte>{buffer}.first(read.bytes);
        visitor(chunk);
        total += read.bytes;
    }
}

} // namespace nxt::rt
