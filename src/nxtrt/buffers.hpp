#pragma once

#include "nxtrt/task.hpp"

#include <algorithm>
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

} // namespace detail

/// Buffered asynchronous writer base.
///
/// The hot-path methods are non-virtual and copy into `buffer_` when there is
/// capacity. An empty buffer is valid and means unbuffered writes: non-empty
/// writes always go through the slow path. Derived writers implement
/// `write_more()`, the slow-path operation that drains bytes to the concrete
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

    hope<void> flush()
    {
        if (end_ == 0)
            return hope<void>::ready();
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
    virtual hope<std::size_t> write_more(
        std::span<const std::byte> src) = 0;

private:
    void append_to_buffer(std::span<const std::byte> bytes)
    {
        std::memcpy(buffer_.data() + end_, bytes.data(), bytes.size());
        end_ += bytes.size();
    }

    task<void> flush_slow()
    {
        auto remaining = buffered();
        while (!remaining.empty()) {
            auto written = co_await write_more(remaining);
            if (written == 0)
                throw buffer_error{"writer made no progress"};
            if (written > remaining.size())
                throw buffer_error{"writer overreported written bytes"};
            remaining = remaining.subspan(written);
        }
        end_ = 0;
    }

    task<void> write_direct(std::span<const std::byte> bytes)
    {
        auto remaining = bytes;
        while (!remaining.empty()) {
            auto written = co_await write_more(remaining);
            if (written == 0)
                throw buffer_error{"writer made no progress"};
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
                co_await flush();
                co_await write_direct(remaining);
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

    std::vector<std::byte> owned_buffer_;
    std::span<std::byte> buffer_;
    std::size_t end_ = 0;
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
    hope<std::size_t> write_more(std::span<const std::byte> src) override
    {
        return write_more_task(src);
    }

    task<std::size_t> write_more_task(std::span<const std::byte> src)
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
    hope<std::size_t> write_more(std::span<const std::byte> src) override
    {
        return write_more_task(src);
    }

    task<std::size_t> write_more_task(std::span<const std::byte> src)
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
    hope<std::size_t> write_more(std::span<const std::byte> src) override
    {
        return write_more_task(src);
    }

    task<std::size_t> write_more_task(std::span<const std::byte> src)
    {
        co_await inner_.write(src);
        co_await inner_.flush();
        if (!src.empty())
            progress_(src.size());
        co_return src.size();
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

/// Buffered asynchronous reader base.
///
/// The hot-path methods are non-virtual and operate directly on `buffer_`,
/// `seek_`, and `end_`. Derived readers only implement `read_more()`, the
/// slow-path operation that appends bytes to `unused_capacity()` and advances
/// `end_` by the returned byte count.
///
/// Unlike Zig's `std.Io.Reader`, this reader's virtual boundary is refill-style
/// rather than stream/readv-style: source bytes must first land in `buffer_`
/// before borrowed-span APIs such as `peek()` and `take()` can expose them.
/// That means a reader needs at least one byte of storage, even if the caller
/// wants every operation to use the cold path.
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

protected:
    [[nodiscard]] std::span<std::byte> buffer_storage() noexcept
    {
        return buffer_;
    }

    void append_read_bytes(std::size_t n)
    {
        if (n > unused_capacity().size())
            throw buffer_error{"source overfilled read buffer"};
        end_ += n;
    }

private:
    virtual hope<read_result> read_more() = 0;

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
    hope<read_result> read_more() override
    {
        return read_more_task();
    }

    task<read_result> read_more_task()
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw buffer_error{"reader buffer is full"};

        auto result = co_await std::invoke(read_, dst);
        auto out = read_result{};
        if constexpr (std::same_as<decltype(result), read_result>) {
            out = result;
        } else {
            out = read_result{
                .bytes = result,
                .eof = result == 0,
            };
        }
        append_read_bytes(out.bytes);
        co_return out;
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
    hope<read_result> read_more() override
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw buffer_error{"reader buffer is full"};

        auto written = std::size_t{0};
        while (written < dst.size() && chunk_ != end_) {
            auto chunk = detail::reader_chunk_bytes(*chunk_);
            auto rest = chunk.subspan(offset_);
            auto n = std::min(dst.size() - written, rest.size());
            std::memcpy(dst.data() + written, rest.data(), n);

            written += n;
            offset_ += n;
            if (offset_ == chunk.size()) {
                ++chunk_;
                offset_ = 0;
            }
        }

        append_read_bytes(written);
        return hope<read_result>::ready(
            read_result{
                .bytes = written,
                .eof = chunk_ == end_,
            });
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
    hope<read_result> read_more() override
    {
        return read_more_task();
    }

    task<read_result> read_more_task()
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw buffer_error{"reader buffer is full"};

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
        append_read_bytes(n);
        co_return read_result{
            .bytes = n,
            .eof = n == 0,
        };
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
    hope<read_result> read_more() override
    {
        return read_more_task();
    }

    task<read_result> read_more_task()
    {
        auto dst = unused_capacity();
        if (dst.empty())
            throw buffer_error{"reader buffer is full"};

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
        append_read_bytes(n);
        co_return read_result{
            .bytes = n,
            .eof = n == 0,
        };
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
    hope<read_result> read_more() override
    {
        auto chunk = inner_.take_some(unused_capacity().size());
        if (chunk.is_ready())
            return hope<read_result>::ready(copy_chunk(chunk.take_ready()));
        return read_more_slow(std::move(chunk));
    }

    task<read_result> read_more_slow(
        hope<std::optional<std::span<const std::byte>>> chunk)
    {
        co_return copy_chunk(co_await std::move(chunk));
    }

    read_result copy_chunk(std::optional<std::span<const std::byte>> chunk)
    {
        if (!chunk)
            return read_result{.bytes = 0, .eof = true};
        auto dst = unused_capacity();
        if (chunk->size() > dst.size())
            throw buffer_error{"metered reader overfilled buffer"};
        std::memcpy(dst.data(), chunk->data(), chunk->size());
        append_read_bytes(chunk->size());
        if (chunk->size() > 0)
            progress_(chunk->size());
        return read_result{.bytes = chunk->size(), .eof = false};
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
