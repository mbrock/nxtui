#pragma once

#include "nxt/rt/task.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
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

namespace nxt::rt {

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

/// Runtime-polymorphic byte source.
class byte_source
{
public:
    virtual ~byte_source() = default;

    virtual task<read_result> read_some(std::span<std::byte> dst) = 0;
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

/// Byte source backed by a callable returning `task<read_result>` or
/// `task<std::size_t>`. Count-only reads treat zero bytes as EOF.
template<byte_read_task Read>
class task_byte_source final : public byte_source
{
public:
    explicit task_byte_source(Read read)
        : read_(std::move(read))
    {}

    task<read_result> read_some(std::span<std::byte> dst) override
    {
        auto result = co_await std::invoke(read_, dst);
        if constexpr (std::same_as<decltype(result), read_result>) {
            co_return result;
        } else {
            co_return read_result{
                .bytes = result,
                .eof = result == 0,
            };
        }
    }

private:
    Read read_;
};

/// Runtime-polymorphic byte sink.
class byte_sink
{
public:
    virtual ~byte_sink() = default;

    virtual task<std::size_t> write_some(std::span<const std::byte> src) = 0;
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
        auto n = std::size_t{0};
        while (true) {
            try {
                n = co_await op::read_some{
                    .fd = fd_,
                    .buffer = dst,
                    .offset = -1,
                };
                break;
            } catch (const interrupted_system_call &) {
            }
        }
        co_return read_result{
            .bytes = n,
            .eof = n == 0,
        };
    }

private:
    int fd_ = -1;
};

/// Byte sink for a file descriptor.
class fd_sink final : public byte_sink
{
public:
    explicit fd_sink(int fd) noexcept
        : fd_(fd)
    {}

    task<std::size_t> write_some(std::span<const std::byte> src) override
    {
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

private:
    int fd_ = -1;
};

inline fd_sink standard_output() noexcept
{
    return fd_sink{STDOUT_FILENO};
}

/// Byte source for a connected socket.
class socket_source final : public byte_source
{
public:
    explicit socket_source(int fd, int flags = 0) noexcept
        : fd_(fd)
        , flags_(flags)
    {}

    task<read_result> read_some(std::span<std::byte> dst) override
    {
        auto n = std::size_t{0};
        while (true) {
            try {
                n = co_await op::recv_some{
                    .fd = fd_,
                    .buffer = dst,
                    .flags = flags_,
                };
                break;
            } catch (const interrupted_system_call &) {
            }
        }
        co_return read_result{
            .bytes = n,
            .eof = n == 0,
        };
    }

private:
    int fd_ = -1;
    int flags_ = 0;
};

/// Byte sink for a connected socket.
class socket_sink final : public byte_sink
{
public:
    explicit socket_sink(int fd, int flags = 0) noexcept
        : fd_(fd)
        , flags_(flags)
    {}

    task<std::size_t> write_some(std::span<const std::byte> src) override
    {
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

private:
    int fd_ = -1;
    int flags_ = 0;
};

template<typename Inner, typename Progress>
class metered_byte_source final : public byte_source
{
public:
    metered_byte_source(Inner inner, Progress progress)
        : inner_(std::move(inner))
        , progress_(std::move(progress))
    {
    }

    task<read_result> read_some(std::span<std::byte> dst) override
    {
        auto result = co_await inner_.read_some(dst);
        if (result.bytes > 0)
            std::invoke(progress_, result.bytes);
        co_return result;
    }

private:
    Inner inner_;
    Progress progress_;
};

template<typename Inner, typename Progress>
auto meter_source(Inner inner, Progress progress)
{
    return metered_byte_source<Inner, Progress>{
        std::move(inner), std::move(progress)};
}

template<typename Inner, typename Progress>
class metered_byte_sink final : public byte_sink
{
public:
    metered_byte_sink(Inner inner, Progress progress)
        : inner_(std::move(inner))
        , progress_(std::move(progress))
    {
    }

    task<std::size_t> write_some(std::span<const std::byte> src) override
    {
        auto written = co_await inner_.write_some(src);
        if (written > 0)
            std::invoke(progress_, written);
        co_return written;
    }

private:
    Inner inner_;
    Progress progress_;
};

template<typename Inner, typename Progress>
auto meter_sink(Inner inner, Progress progress)
{
    return metered_byte_sink<Inner, Progress>{
        std::move(inner), std::move(progress)};
}

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

inline task<void> write_all(
    byte_sink & sink,
    std::span<const std::byte> bytes)
{
    auto remaining = bytes;
    while (!remaining.empty()) {
        auto written = co_await sink.write_some(remaining);
        if (written == 0)
            throw buffer_error{"sink write made no progress"};
        if (written > remaining.size())
            throw buffer_error{"sink overreported written bytes"};
        remaining = remaining.subspan(written);
    }
}

inline task<void> write_all(byte_sink & sink, std::string text)
{
    co_await write_all(sink, as_bytes(std::string_view{text}));
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

} // namespace detail

/// Buffered asynchronous writer over a byte sink.
template<typename Sink = byte_sink>
class byte_writer
{
public:
    byte_writer(Sink & sink, std::span<std::byte> buffer)
        : sink_(&sink)
        , buffer_(buffer)
    {
        if (buffer.empty())
            throw buffer_error{"writer buffer is empty"};
    }

    byte_writer(Sink & sink, std::size_t buffer_size)
        : owned_buffer_(buffer_size)
        , sink_(&sink)
        , buffer_(owned_buffer_)
    {
        if (owned_buffer_.empty())
            throw buffer_error{"writer buffer is empty"};
    }

    byte_writer(Sink && sink, std::size_t buffer_size)
        : owned_sink_(std::make_unique<Sink>(std::move(sink)))
        , owned_buffer_(buffer_size)
        , sink_(owned_sink_.get())
        , buffer_(owned_buffer_)
    {
        if (owned_buffer_.empty())
            throw buffer_error{"writer buffer is empty"};
    }

    byte_writer(const byte_writer &) = delete;
    byte_writer & operator=(const byte_writer &) = delete;

    byte_writer(byte_writer && other) noexcept
        : owned_sink_(std::move(other.owned_sink_))
        , owned_buffer_(std::move(other.owned_buffer_))
        , sink_(owned_sink_ ? owned_sink_.get() : other.sink_)
        , buffer_(owned_buffer_.empty()
            ? other.buffer_
            : std::span<std::byte>{owned_buffer_})
        , end_(other.end_)
    {
        other.sink_ = nullptr;
        other.buffer_ = {};
        other.end_ = 0;
    }

    byte_writer & operator=(byte_writer && other) noexcept
    {
        if (this != &other) {
            owned_sink_ = std::move(other.owned_sink_);
            owned_buffer_ = std::move(other.owned_buffer_);
            sink_ = owned_sink_ ? owned_sink_.get() : other.sink_;
            buffer_ = owned_buffer_.empty()
                ? other.buffer_
                : std::span<std::byte>{owned_buffer_};
            end_ = other.end_;

            other.sink_ = nullptr;
            other.buffer_ = {};
            other.end_ = 0;
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::byte> buffered() const noexcept
    {
        return std::span<const std::byte>{buffer_}.first(end_);
    }

    [[nodiscard]] std::size_t buffered_size() const noexcept
    {
        return end_;
    }

    [[nodiscard]] std::span<std::byte> unused_capacity() noexcept
    {
        return buffer_.subspan(end_);
    }

    task<void> flush()
    {
        co_await nxt::rt::write_all(*sink_, buffered());
        end_ = 0;
    }

    task<void> write(std::span<const std::byte> bytes)
    {
        auto remaining = bytes;
        while (!remaining.empty()) {
            if (remaining.size() >= buffer_.size()) {
                co_await flush();
                co_await nxt::rt::write_all(*sink_, remaining);
                co_return;
            }

            if (unused_capacity().empty())
                co_await flush();

            auto n = std::min(unused_capacity().size(), remaining.size());
            std::memcpy(buffer_.data() + end_, remaining.data(), n);
            end_ += n;
            remaining = remaining.subspan(n);
        }
    }

    task<void> write(std::string text)
    {
        co_await write(as_bytes(std::string_view{text}));
    }

    task<void> write(std::string_view text)
    {
        co_await write(as_bytes(text));
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

private:
    std::unique_ptr<Sink> owned_sink_;
    std::vector<std::byte> owned_buffer_;
    Sink * sink_;
    std::span<std::byte> buffer_;
    std::size_t end_ = 0;
};

inline byte_writer<fd_sink> standard_output_writer(
    std::size_t buffer_size = 4096)
{
    return byte_writer<fd_sink>{standard_output(), buffer_size};
}

template<typename Sink, typename Chunks>
    requires detail::byte_writer_chunk<Chunks>
        || detail::byte_writer_chunk_range<Chunks>
inline task<void> write_all(
    Sink && sink,
    Chunks && chunks,
    std::size_t buffer_size = 4096)
{
    auto writer = byte_writer<std::remove_reference_t<Sink>>{
        std::forward<Sink>(sink),
        buffer_size,
    };
    co_await writer.write_all(std::forward<Chunks>(chunks));
}

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

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    task<T> peek_struct()
    {
        auto bytes = co_await peek(sizeof(T));
        auto value = T{};
        std::memcpy(&value, bytes.data(), sizeof(T));
        co_return value;
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

    task<std::string_view> take_string_view(std::size_t n)
    {
        co_return as_string_view(co_await take(n));
    }

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    task<std::optional<T>> take_struct()
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

        auto value = T{};
        std::memcpy(&value, buffered().data(), sizeof(T));
        toss(sizeof(T));
        co_return value;
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
