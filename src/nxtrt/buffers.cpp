#include "nxtrt/buffers.hpp"

#include "nxtrt/wish_ops.hpp"

#include <unistd.h>

namespace nxtrt {

template class sink<std::byte>;
template class feed<std::byte>;
template class fixed_sink<std::byte>;
template class discarding_sink<std::byte>;

task<std::size_t>
send_some(int fd, std::span<const std::byte> buffer, int flags)
{
    co_return co_await op::send_some{
        .fd = fd,
        .buffer = buffer,
        .flags = flags,
    };
}

task<std::size_t>
write_some(int fd, std::span<const std::byte> buffer, off_t offset)
{
    co_return co_await op::write_some{
        .fd = fd,
        .buffer = buffer,
        .offset = offset,
    };
}

hope<std::size_t>
fd_sink::drain_more(value_chunk_view chunks, std::size_t splat)
{
    return drain_more_task(chunks, splat);
}

task<std::size_t>
fd_sink::drain_more_task(value_chunk_view chunks, std::size_t splat)
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

std::span<const std::byte>
fd_sink::first_nonempty(value_chunk_view chunks, std::size_t splat) noexcept
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

fd_sink standard_output(std::size_t buffer_size)
{
    return fd_sink{STDOUT_FILENO, buffer_size};
}

fd_sink standard_output_sink(std::size_t buffer_size)
{
    return standard_output(buffer_size);
}

hope<std::size_t>
socket_sink::drain_more(value_chunk_view chunks, std::size_t splat)
{
    return drain_more_task(chunks, splat);
}

task<std::size_t>
socket_sink::drain_more_task(value_chunk_view chunks, std::size_t splat)
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

std::span<const std::byte>
socket_sink::first_nonempty(value_chunk_view chunks, std::size_t splat) noexcept
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

task<std::size_t>
fd_source::read_into(junk<std::byte> dst)
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

task<std::size_t>
socket_source::read_into(junk<std::byte> dst)
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

task<std::size_t>
stream_all(bytefeed & reader, bytesink & writer)
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
