#pragma once

#include <nxt/mt/wire.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace nxt::mt {

struct quick_ack
{
    std::uint32_t token = 0;
};

struct abridged_payload
{
    std::span<const std::byte> bytes;
};

using abridged_frame = std::variant<abridged_payload, quick_ack>;

inline constexpr auto abridged_client_prefix = std::byte{0xef};
inline constexpr auto abridged_max_words = std::size_t{16'777'216};

inline std::size_t abridged_frame_size(std::span<const std::byte> payload)
{
    if (payload.size() % 4 != 0)
        throw protocol_error{"abridged payload is not word-aligned"};
    auto words = payload.size() / 4;
    if (words >= abridged_max_words)
        throw protocol_error{"abridged payload is too large"};
    return payload.size() + (words < 0x7f ? 1 : 4);
}

inline void write_abridged_client_prefix(byte_writer & out)
{
    out.put(abridged_client_prefix);
}

inline void write_abridged_frame(
    byte_writer & out,
    std::span<const std::byte> payload,
    bool quick_ack_requested = false)
{
    auto words = payload.size() / 4;
    (void)abridged_frame_size(payload);

    if (words < 0x7f) {
        out.put_u8(static_cast<std::uint8_t>(
            words + (quick_ack_requested ? 0x80 : 0)));
    } else {
        out.put_u8(quick_ack_requested ? 0xff : 0x7f);
        out.put_le(words, 3);
    }
    out.put(payload);
}

inline abridged_frame read_abridged_frame(byte_reader & input)
{
    auto first = input.u8();
    if (first >= 0x80) {
        auto raw = static_cast<std::uint32_t>(first)
                   | (static_cast<std::uint32_t>(input.u8()) << 8)
                   | (static_cast<std::uint32_t>(input.u8()) << 16)
                   | (static_cast<std::uint32_t>(input.u8()) << 24);
        return quick_ack{.token = byte_swap32(raw)};
    }

    auto words = std::size_t{first};
    if (first == 0x7f)
        words = input.le(3);
    return abridged_payload{.bytes = input.take(words * 4)};
}

} // namespace nxt::mt
