#pragma once

#include "nxt/rt/task.hpp"

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace nxt::rt {

/// View immutable bytes as text without copying.
inline std::string_view as_string_view(std::span<const std::byte> bytes) noexcept
{
    return {
        reinterpret_cast<const char *>(bytes.data()),
        bytes.size_bytes(),
    };
}

/// Runtime-polymorphic byte source.
///
/// This mirrors the `read_some` shape from `src/nxtio/buffers.hpp`, but uses a
/// vtable so higher-level buffer code can be written against one small runtime
/// interface while the source chooses how it waits for platform I/O.
class byte_source
{
public:
    virtual ~byte_source() = default;

    virtual task<std::size_t> read_some(std::span<std::byte> dst) = 0;
};

/// Borrowed in-memory source exposed through the byte-source vtable.
class string_source final : public byte_source
{
public:
    explicit string_source(std::span<const std::string_view> chunks)
        : chunks_(chunks)
    {}

    task<std::size_t> read_some(std::span<std::byte> dst) override
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

        co_return written;
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

    task<std::size_t> read_some(std::span<std::byte> dst) override
    {
        co_return co_await read_some_wish{
            .fd = fd_,
            .buffer = dst,
            .offset = -1,
        };
    }

private:
    int fd_ = -1;
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
        auto n = co_await source.read_some(buffer);
        if (n == 0)
            co_return total;

        auto chunk = std::span<const std::byte>{buffer}.first(n);
        visitor(chunk);
        total += n;
    }
}

} // namespace nxt::rt
