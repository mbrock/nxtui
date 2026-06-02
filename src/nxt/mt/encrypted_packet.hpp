#pragma once

#include <nxt/mt/crypto.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nxt::mt {

using bytes = std::vector<std::byte>;

struct encrypted_packet
{
    std::int64_t salt = 0;
    std::int64_t session_id = 0;
    std::int64_t message_id = 0;
    std::int32_t seq_no = 0;
    bytes body;
    bytes padding;
};

inline std::size_t default_encrypted_padding_size(std::size_t plaintext_size)
{
    return 16 + (16 - (plaintext_size % 16));
}

inline void validate_encrypted_packet_padding(
    std::span<const std::byte> body,
    std::span<const std::byte> padding)
{
    if (body.size() % 4 != 0)
        throw protocol_error{"invalid encrypted packet body"};
    if (padding.size() < 12 || padding.size() > 1024)
        throw protocol_error{"invalid encrypted packet padding"};
    if ((32 + body.size() + padding.size()) % 16 != 0)
        throw protocol_error{"invalid encrypted packet padding alignment"};
}

inline bytes encode_encrypted_plaintext(const encrypted_packet & packet)
{
    validate_encrypted_packet_padding(packet.body, packet.padding);

    auto out = bytes(32 + packet.body.size() + packet.padding.size());
    auto writer = byte_writer{out};
    writer.put_le(static_cast<std::uint64_t>(packet.salt), 8);
    writer.put_le(static_cast<std::uint64_t>(packet.session_id), 8);
    writer.put_le(static_cast<std::uint64_t>(packet.message_id), 8);
    writer.put_le(static_cast<std::uint32_t>(packet.seq_no), 4);
    writer.put_le(static_cast<std::uint32_t>(packet.body.size()), 4);
    writer.put(packet.body);
    writer.put(packet.padding);
    return out;
}

inline encrypted_packet decode_encrypted_plaintext(
    std::span<const std::byte> plaintext,
    std::optional<std::int64_t> expected_session_id = std::nullopt)
{
    auto reader = byte_reader{plaintext};
    auto packet = encrypted_packet{};
    packet.salt = reader.i64_le();
    packet.session_id = reader.i64_le();
    packet.message_id = reader.i64_le();
    packet.seq_no = reader.i32_le();

    auto body_size = reader.i32_le();
    if (body_size < 0 || body_size % 4 != 0)
        throw protocol_error{"invalid encrypted packet body length"};
    auto body = reader.take(static_cast<std::size_t>(body_size));
    packet.body = bytes{body.begin(), body.end()};
    auto padding = reader.remaining();
    packet.padding = bytes{padding.begin(), padding.end()};
    validate_encrypted_packet_padding(packet.body, packet.padding);

    if (expected_session_id && packet.session_id != *expected_session_id)
        throw protocol_error{"session id mismatch"};
    return packet;
}

inline bytes encode_encrypted_packet(
    encrypted_packet packet,
    const auth_key & key,
    sender from,
    std::span<const std::byte> padding_bytes)
{
    if (packet.padding.empty()) {
        auto padding_size = default_encrypted_padding_size(
            32 + packet.body.size());
        if (padding_bytes.size() < padding_size)
            throw protocol_error{"insufficient encrypted packet padding"};
        packet.padding = bytes{
            padding_bytes.begin(),
            padding_bytes.begin() + static_cast<std::ptrdiff_t>(padding_size),
        };
    }

    auto plaintext = encode_encrypted_plaintext(packet);
    auto out = bytes(encrypted_payload_size(plaintext));
    encrypt_padded(plaintext, key, from, out);
    return out;
}

inline encrypted_packet decode_encrypted_packet(
    std::span<const std::byte> payload,
    const auth_key & key,
    sender from,
    std::optional<std::int64_t> expected_session_id = std::nullopt)
{
    if (payload.size() < 24)
        throw protocol_error{"short encrypted payload"};
    auto plaintext = bytes(payload.size() - 24);
    auto opened = decrypt_padded(payload, key, from, plaintext);
    return decode_encrypted_plaintext(opened, expected_session_id);
}

} // namespace nxt::mt
