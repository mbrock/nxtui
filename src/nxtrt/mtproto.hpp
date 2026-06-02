#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/task.hpp"
#include "nxt/mt/message.hpp"
#include "nxt/mt/tl.hpp"
#include "nxt/mt/transport.hpp"

#include <array>
#include <bit>
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

inline std::uint64_t abridged_le(
    byte_chunks<const std::byte> bytes,
    std::size_t offset,
    std::size_t width)
{
    if (width > 8)
        throw nxt::mt::protocol_error{"little-endian integer is too wide"};

    auto value = std::uint64_t{0};
    for (auto i = std::size_t{0}; i < width; i++) {
        value |= static_cast<std::uint64_t>(abridged_u8(bytes, offset + i))
              << (i * 8);
    }
    return value;
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

struct plain_abridged_frame_view
{
    std::optional<std::uint32_t> quick_ack_token;
    std::uint64_t message_id = 0;
    byte_chunks<const std::byte> body;

    [[nodiscard]] bool is_quick_ack() const noexcept
    {
        return quick_ack_token.has_value();
    }

    [[nodiscard]] bool is_message() const noexcept
    {
        return !quick_ack_token.has_value();
    }

    static chop_scan_result<plain_abridged_frame_view> scan(
        byte_chunks<const std::byte> bytes)
    {
        auto transport = abridged_frame_view::scan(bytes);
        auto * complete = std::get_if<frame_chop<abridged_frame_view>>(
            &transport);
        if (complete == nullptr)
            return std::get<chop_need_more>(transport);

        if (complete->frame.is_quick_ack()) {
            return frame_chop<plain_abridged_frame_view>{
                .extent = complete->extent,
                .frame = plain_abridged_frame_view{
                    .quick_ack_token = complete->frame.quick_ack_token,
                    .message_id = 0,
                    .body = {},
                },
            };
        }

        auto payload = complete->frame.payload;
        if (payload.size() < 20)
            throw nxt::mt::protocol_error{"short plain MTProto message"};
        if (detail::abridged_le(payload, 0, 8) != 0)
            throw nxt::mt::protocol_error{
                "encrypted message is not a plain message",
            };

        auto body_size = static_cast<std::size_t>(
            detail::abridged_le(payload, 16, 4));
        auto body_offset = std::size_t{20};
        if (body_size > payload.size() - body_offset)
            throw nxt::mt::protocol_error{"short plain MTProto body"};
        if (payload.size() != body_offset + body_size)
            throw nxt::mt::protocol_error{"trailing plain message bytes"};

        return frame_chop<plain_abridged_frame_view>{
            .extent = complete->extent,
            .frame = plain_abridged_frame_view{
                .quick_ack_token = std::nullopt,
                .message_id = detail::abridged_le(payload, 8, 8),
                .body = payload.subspan(body_offset, body_size),
            },
        };
    }
};

using plain_abridged_reel = reel<std::byte, plain_abridged_frame_view>;

class chunk_reader
{
public:
    explicit chunk_reader(byte_chunks<const std::byte> input)
        : input_(input)
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return offset_ == input_.size();
    }

    [[nodiscard]] std::size_t remaining_size() const noexcept
    {
        return input_.size() - offset_;
    }

    [[nodiscard]] byte_chunks<const std::byte> remaining() const
    {
        return input_.subspan(offset_);
    }

    byte_chunks<const std::byte> take(std::size_t count)
    {
        if (count > remaining_size())
            throw nxt::mt::protocol_error{"short MTProto input"};
        auto out = input_.subspan(offset_, count);
        offset_ += count;
        return out;
    }

    std::uint8_t u8()
    {
        return detail::abridged_u8(take(1), 0);
    }

    std::uint64_t le(std::size_t width)
    {
        auto input = take(width);
        return detail::abridged_le(input, 0, width);
    }

    std::uint32_t u32_le()
    {
        return static_cast<std::uint32_t>(le(4));
    }

    std::uint64_t u64_le()
    {
        return le(8);
    }

    std::int32_t i32_le()
    {
        return std::bit_cast<std::int32_t>(u32_le());
    }

    std::int64_t i64_le()
    {
        return std::bit_cast<std::int64_t>(u64_le());
    }

private:
    byte_chunks<const std::byte> input_;
    std::size_t offset_ = 0;
};

class tl_chunk_reader
{
public:
    explicit tl_chunk_reader(byte_chunks<const std::byte> input)
        : input_(input)
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return input_.empty();
    }

    [[nodiscard]] byte_chunks<const std::byte> remaining() const
    {
        return input_.remaining();
    }

    std::int32_t int_()
    {
        return input_.i32_le();
    }

    std::uint64_t long_()
    {
        return input_.u64_le();
    }

    std::int64_t signed_long()
    {
        return input_.i64_le();
    }

    bool bool_()
    {
        auto constructor = input_.u32_le();
        if (constructor == nxt::mt::tl::bool_true)
            return true;
        if (constructor == nxt::mt::tl::bool_false)
            return false;
        throw nxt::mt::protocol_error{"invalid TL bool"};
    }

    byte_chunks<const std::byte> int128()
    {
        return input_.take(16);
    }

    byte_chunks<const std::byte> int256()
    {
        return input_.take(32);
    }

    byte_chunks<const std::byte> bytes()
    {
        auto size = std::size_t{input_.u8()};
        auto header = std::size_t{1};
        if (size == 254) {
            size = input_.le(3);
            header = 4;
        }

        auto value = input_.take(size);
        input_.take(nxt::mt::tl_padding_size(header + size));
        return value;
    }

private:
    chunk_reader input_;
};

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
