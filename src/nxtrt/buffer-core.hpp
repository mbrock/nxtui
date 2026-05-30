#pragma once

#include "nxtrt/task.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

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

template<typename T, std::size_t Inline = 2>
class buffer_chunks
{
public:
    using value_type = T;
    using chunk_type = std::span<T>;

    buffer_chunks() = default;

    buffer_chunks(chunk_type chunk)
    {
        append(chunk);
    }

    buffer_chunks(std::span<const chunk_type> chunks)
    {
        for (auto chunk : chunks)
            append(chunk);
    }

    [[nodiscard]] std::span<const chunk_type> chunks() const noexcept
    {
        return std::span{chunks_.data(), count_};
    }

    [[nodiscard]] auto begin() const noexcept
    {
        return chunks().begin();
    }

    [[nodiscard]] auto end() const noexcept
    {
        return chunks().end();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size() == 0;
    }

    [[nodiscard]] std::size_t chunk_count() const noexcept
    {
        return count_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        auto total = std::size_t{0};
        for (auto chunk : chunks())
            total += chunk.size();
        return total;
    }

    [[nodiscard]] buffer_chunks first(std::size_t n) const
    {
        auto out = buffer_chunks{};
        for (auto chunk : chunks()) {
            if (n == 0)
                break;
            auto take = std::min(n, chunk.size());
            out.append(chunk.first(take));
            n -= take;
        }
        return out;
    }

    [[nodiscard]] std::optional<chunk_type> single_span() const noexcept
    {
        if (count_ == 0)
            return chunk_type{};
        if (count_ == 1)
            return chunks_[0];
        return std::nullopt;
    }

private:
    void append(chunk_type chunk)
    {
        if (chunk.empty())
            return;
        if (count_ == Inline)
            throw buffer_error{"too many buffer chunks"};
        chunks_[count_++] = chunk;
    }

    std::array<chunk_type, Inline> chunks_{};
    std::size_t count_ = 0;
};

template<typename T = std::byte, std::size_t Inline = 2>
using byte_chunks = buffer_chunks<T, Inline>;

namespace detail {

template<typename T>
[[nodiscard]] buffer_chunks<T> ring_chunks(
    T * data,
    std::size_t capacity,
    std::size_t seek,
    std::size_t size) noexcept
{
    if (size == 0)
        return {};

    auto first = std::min(size, capacity - seek);
    if (first == size)
        return buffer_chunks<T>{std::span<T>{data + seek, first}};

    auto chunks = std::array{
        std::span<T>{data + seek, first},
        std::span<T>{data, size - first},
    };
    return buffer_chunks<T>{std::span{chunks}};
}

template<typename T>
[[nodiscard]] std::size_t ring_write_index(
    std::size_t capacity,
    std::size_t seek,
    std::size_t size) noexcept
{
    return (seek + size) % capacity;
}

template<typename T>
[[nodiscard]] std::span<T> ring_unused_capacity(
    T * data,
    std::size_t capacity,
    std::size_t seek,
    std::size_t size) noexcept
{
    if (size == capacity)
        return {};

    auto write = ring_write_index<T>(capacity, seek, size);
    auto n = write < seek
        ? seek - write
        : capacity - write;
    return std::span{data + write, std::min(n, capacity - size)};
}

} // namespace detail

} // namespace nxtrt
