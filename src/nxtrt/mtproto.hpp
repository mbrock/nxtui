#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/task.hpp"
#include "nxt/mt/message.hpp"
#include "nxt/mt/transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace nxtrt::mtproto {

namespace detail {

inline std::uint8_t abridged_u8(
    byte_chunks<const std::byte> bytes,
    std::size_t index)
{
    for (auto chunk : bytes) {
        if (index < chunk.size())
            return std::to_integer<std::uint8_t>(chunk[index]);
        index -= chunk.size();
    }
    throw nxt::mt::protocol_error{"short abridged frame"};
}

} // namespace detail

struct abridged_frame_view
{
    std::optional<std::uint32_t> quick_ack_token;
    byte_chunks<const std::byte> payload;

    [[nodiscard]] bool is_quick_ack() const noexcept
    {
        return quick_ack_token.has_value();
    }

    [[nodiscard]] bool is_payload() const noexcept
    {
        return !quick_ack_token.has_value();
    }

    static chop_scan_result<abridged_frame_view> scan(
        byte_chunks<const std::byte> bytes)
    {
        if (bytes.size() < 1)
            return chop_need_more{.minimum_buffered = 1};

        auto first = detail::abridged_u8(bytes, 0);
        if (first >= 0x80) {
            if (bytes.size() < 4)
                return chop_need_more{.minimum_buffered = 4};

            auto raw = static_cast<std::uint32_t>(first)
                     | (static_cast<std::uint32_t>(
                            detail::abridged_u8(bytes, 1))
                        << 8)
                     | (static_cast<std::uint32_t>(
                            detail::abridged_u8(bytes, 2))
                        << 16)
                     | (static_cast<std::uint32_t>(
                            detail::abridged_u8(bytes, 3))
                        << 24);
            return frame_chop<abridged_frame_view>{
                .extent = 4,
                .frame = abridged_frame_view{
                    .quick_ack_token = nxt::mt::byte_swap32(raw),
                    .payload = {},
                },
            };
        }

        auto header = std::size_t{1};
        auto words = std::size_t{first};
        if (first == 0x7f) {
            if (bytes.size() < 4)
                return chop_need_more{.minimum_buffered = 4};

            words = static_cast<std::size_t>(
                detail::abridged_u8(bytes, 1)
                | (static_cast<std::uint32_t>(
                       detail::abridged_u8(bytes, 2))
                   << 8)
                | (static_cast<std::uint32_t>(
                       detail::abridged_u8(bytes, 3))
                   << 16));
            header = 4;
        }

        if (words >= nxt::mt::abridged_max_words)
            throw nxt::mt::protocol_error{"abridged payload is too large"};

        auto payload_size = words * 4;
        auto extent = header + payload_size;
        if (bytes.size() < extent)
            return chop_need_more{.minimum_buffered = extent};

        return frame_chop<abridged_frame_view>{
            .extent = extent,
            .frame = abridged_frame_view{
                .quick_ack_token = std::nullopt,
                .payload = bytes.subspan(header, payload_size),
            },
        };
    }
};

using abridged_reel = reel<std::byte, abridged_frame_view>;

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

inline task<void> write_plain_abridged_frame(
    bytesink & writer,
    std::span<const std::byte> body,
    std::optional<std::uint64_t> & last_message_id,
    std::span<std::byte> storage,
    std::uint64_t now_ns = nxt::mt::now_nanoseconds())
{
    auto message_size = nxt::mt::plain_message_size(body);
    if (storage.size() < message_size)
        throw nxt::mt::protocol_error{"plain message storage is too small"};

    auto message_id = nxt::mt::next_message_id(last_message_id, now_ns);
    auto message_writer = nxt::mt::byte_writer{storage.first(message_size)};
    nxt::mt::write_plain_message(
        message_writer,
        nxt::mt::plain_message_view{
            .message_id = message_id,
            .body = body,
        });

    co_await write_abridged_frame(writer, message_writer.written());
    last_message_id = message_id;
}

template<typename Reader>
task<nxt::mt::plain_message_view> read_plain_abridged_frame(Reader & reader)
{
    auto frame = co_await read_abridged_frame(reader);
    co_return nxt::mt::read_plain_message(frame);
}

} // namespace nxtrt::mtproto
