#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/task.hpp"
#include "nxt/mt/transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nxtrt::mtproto {

inline task<void> write_abridged_client_prefix(bytesink & writer)
{
    auto prefix = std::array{nxt::mt::abridged_client_prefix};
    co_await writer.write(std::span<const std::byte>{prefix});
    co_await writer.flush();
}

inline task<void> write_abridged_frame(
    bytesink & writer,
    std::span<const std::byte> payload,
    bool quick_ack_requested = false)
{
    auto header = std::array<std::byte, 4>{};
    auto header_writer = nxt::mt::byte_writer{header};
    auto words = payload.size() / 4;
    (void)nxt::mt::abridged_frame_size(payload);
    if (words < 0x7f) {
        header_writer.put_u8(static_cast<std::uint8_t>(
            words + (quick_ack_requested ? 0x80 : 0)));
    } else {
        header_writer.put_u8(quick_ack_requested ? 0xff : 0x7f);
        header_writer.put_le(words, 3);
    }

    auto spans = std::array{
        std::span<const std::byte>{header_writer.written()},
        payload,
    };
    auto chunks = byte_chunks<const std::byte, 2>{std::span{spans}};
    co_await writer.write(chunks);
    co_await writer.flush();
}

template<typename Reader>
task<std::span<const std::byte>> read_abridged_frame(Reader & reader)
{
    while (true) {
        auto first_byte = co_await reader.take(1);
        auto first = std::to_integer<std::uint8_t>(first_byte[0]);
        if (first >= 0x80) {
            (void)co_await reader.take(3);
            continue;
        }

        auto words = std::size_t{first};
        if (first == 0x7f) {
            auto extended = co_await reader.take(3);
            words = static_cast<std::size_t>(
                std::to_integer<std::uint8_t>(extended[0])
                | (static_cast<std::uint32_t>(
                       std::to_integer<std::uint8_t>(extended[1]))
                   << 8)
                | (static_cast<std::uint32_t>(
                       std::to_integer<std::uint8_t>(extended[2]))
                   << 16));
        }

        co_return co_await reader.take(words * 4);
    }
}

} // namespace nxtrt::mtproto
