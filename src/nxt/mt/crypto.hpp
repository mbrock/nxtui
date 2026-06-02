#pragma once

#include <nxt/crypto.hpp>
#include <nxt/mt/wire.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nxt::mt {

enum class sender
{
    client,
    server,
};

struct auth_key
{
    std::array<std::byte, 256> data{};
    std::array<std::byte, 8> aux_hash{};
    std::array<std::byte, 8> id{};

    [[nodiscard]] std::uint64_t id_integer() const noexcept
    {
        auto value = std::uint64_t{0};
        for (auto i = std::size_t{0}; i < id.size(); i++) {
            value |= static_cast<std::uint64_t>(
                         std::to_integer<std::uint8_t>(id[i]))
                     << (i * 8);
        }
        return value;
    }
};

struct aes_key_iv
{
    std::array<std::byte, 32> key{};
    std::array<std::byte, 32> iv{};
};

inline std::size_t sender_offset(sender from) noexcept
{
    return from == sender::client ? 0 : 8;
}

inline auth_key make_auth_key(std::span<const std::byte> data)
{
    if (data.size() != 256)
        throw protocol_error{"invalid auth key length"};

    auto out = auth_key{};
    std::ranges::copy(data, out.data.begin());
    auto digest = nxt::crypto::sha1(data);
    std::ranges::copy(std::span{digest}.first(8), out.aux_hash.begin());
    std::ranges::copy(std::span{digest}.subspan(12, 8), out.id.begin());
    return out;
}

inline std::array<std::byte, 16> calc_new_nonce_hash(
    const auth_key & key,
    std::span<const std::byte> new_nonce,
    std::uint8_t number)
{
    if (new_nonce.size() != 32 || number < 1 || number > 3)
        throw protocol_error{"invalid new nonce hash input"};

    auto state = nxt::crypto::sha1_state{};
    state.update(new_nonce);
    auto number_byte = std::array{std::byte{number}};
    state.update(number_byte);
    state.update(key.aux_hash);

    auto digest = state.finalize();
    auto out = std::array<std::byte, 16>{};
    std::ranges::copy(std::span{digest}.subspan(4, 16), out.begin());
    return out;
}

inline std::array<std::byte, 16> message_key(
    const auth_key & key,
    std::span<const std::byte> plaintext,
    sender from)
{
    auto x = sender_offset(from);
    auto state = nxt::crypto::sha256_state{};
    state.update(std::span<const std::byte>{key.data}.subspan(88 + x, 32));
    state.update(plaintext);

    auto digest = state.finalize();
    auto out = std::array<std::byte, 16>{};
    std::ranges::copy(std::span{digest}.subspan(8, 16), out.begin());
    return out;
}

inline aes_key_iv derive_aes_key_iv(
    const auth_key & key,
    std::span<const std::byte> msg_key,
    sender from)
{
    if (msg_key.size() != 16)
        throw protocol_error{"invalid msg_key"};

    auto x = sender_offset(from);
    auto key_data = std::span<const std::byte>{key.data};

    auto a = nxt::crypto::sha256_state{};
    a.update(msg_key);
    a.update(key_data.subspan(x, 36));
    auto sha256_a = a.finalize();

    auto b = nxt::crypto::sha256_state{};
    b.update(key_data.subspan(40 + x, 36));
    b.update(msg_key);
    auto sha256_b = b.finalize();

    auto out = aes_key_iv{};
    auto key_out = out.key.begin();
    key_out = std::copy_n(sha256_a.begin(), 8, key_out);
    key_out = std::copy_n(sha256_b.begin() + 8, 16, key_out);
    std::copy_n(sha256_a.begin() + 24, 8, key_out);

    auto iv_out = out.iv.begin();
    iv_out = std::copy_n(sha256_b.begin(), 8, iv_out);
    iv_out = std::copy_n(sha256_a.begin() + 8, 16, iv_out);
    std::copy_n(sha256_b.begin() + 24, 8, iv_out);
    return out;
}

inline void aes_ige_encrypt(
    std::span<const std::byte> input,
    const aes_key_iv & key_iv,
    std::span<std::byte> output)
{
    auto ctx = nxt::crypto::aes256_context{key_iv.key};
    nxt::crypto::aes256_ige_encrypt(ctx, key_iv.iv, input, output);
}

inline void aes_ige_decrypt(
    std::span<const std::byte> input,
    const aes_key_iv & key_iv,
    std::span<std::byte> output)
{
    auto ctx = nxt::crypto::aes256_context{key_iv.key};
    nxt::crypto::aes256_ige_decrypt(ctx, key_iv.iv, input, output);
}

inline std::size_t encrypted_payload_size(std::span<const std::byte> plaintext)
{
    return 24 + plaintext.size();
}

inline void encrypt_padded(
    std::span<const std::byte> plaintext,
    const auth_key & key,
    sender from,
    std::span<std::byte> output)
{
    if (plaintext.empty() || plaintext.size() % nxt::crypto::aes_block_len != 0)
        throw protocol_error{"invalid padded plaintext"};
    if (output.size() != encrypted_payload_size(plaintext))
        throw protocol_error{"encrypted payload output has the wrong length"};

    std::ranges::copy(key.id, output.begin());
    auto msg_key = message_key(key, plaintext, from);
    std::ranges::copy(msg_key, output.begin() + 8);
    auto key_iv = derive_aes_key_iv(key, msg_key, from);
    aes_ige_encrypt(plaintext, key_iv, output.subspan(24));
}

inline std::span<std::byte> decrypt_padded(
    std::span<const std::byte> payload,
    const auth_key & key,
    sender from,
    std::span<std::byte> output)
{
    if (payload.size() < 24)
        throw protocol_error{"short encrypted payload"};
    if (!std::ranges::equal(payload.first(8), key.id))
        throw protocol_error{"auth key id mismatch"};

    auto msg_key = payload.subspan(8, 16);
    auto ciphertext = payload.subspan(24);
    if (ciphertext.empty()
        || ciphertext.size() % nxt::crypto::aes_block_len != 0)
        throw protocol_error{"invalid encrypted payload"};
    if (output.size() != ciphertext.size())
        throw protocol_error{"decrypted payload output has the wrong length"};

    auto key_iv = derive_aes_key_iv(key, msg_key, from);
    aes_ige_decrypt(ciphertext, key_iv, output);
    if (!std::ranges::equal(msg_key, message_key(key, output, from)))
        throw protocol_error{"msg_key mismatch"};
    return output;
}

inline aes_key_iv temp_aes_key_iv(
    std::span<const std::byte> server_nonce,
    std::span<const std::byte> new_nonce)
{
    if (server_nonce.size() != 16 || new_nonce.size() != 32)
        throw protocol_error{"invalid nonce"};

    auto a = nxt::crypto::sha1_state{};
    a.update(new_nonce);
    a.update(server_nonce);
    auto hash_a = a.finalize();

    auto b = nxt::crypto::sha1_state{};
    b.update(server_nonce);
    b.update(new_nonce);
    auto hash_b = b.finalize();

    auto c = nxt::crypto::sha1_state{};
    c.update(new_nonce);
    c.update(new_nonce);
    auto hash_c = c.finalize();

    auto out = aes_key_iv{};
    auto key_out = out.key.begin();
    key_out = std::copy(hash_a.begin(), hash_a.end(), key_out);
    std::copy_n(hash_b.begin(), 12, key_out);

    auto iv_out = out.iv.begin();
    iv_out = std::copy_n(hash_b.begin() + 12, 8, iv_out);
    iv_out = std::copy(hash_c.begin(), hash_c.end(), iv_out);
    std::copy_n(new_nonce.begin(), 4, iv_out);
    return out;
}

inline std::size_t encrypted_data_with_hash_size(std::size_t data_size)
{
    auto plaintext_size = nxt::crypto::sha1_len + data_size;
    auto padding = (nxt::crypto::aes_block_len
                    - (plaintext_size % nxt::crypto::aes_block_len))
                   % nxt::crypto::aes_block_len;
    return plaintext_size + padding;
}

inline void encrypt_data_with_hash(
    std::span<const std::byte> data,
    std::span<const std::byte> key,
    std::span<const std::byte> iv,
    std::span<const std::byte> padding_bytes,
    std::span<std::byte> output)
{
    if (output.size() != encrypted_data_with_hash_size(data.size()))
        throw protocol_error{"encrypted data output has the wrong length"};
    auto padding_size = output.size() - nxt::crypto::sha1_len - data.size();
    if (padding_bytes.size() < padding_size)
        throw protocol_error{"insufficient padding bytes"};

    auto digest = nxt::crypto::sha1(data);
    auto cursor = output.begin();
    cursor = std::copy(digest.begin(), digest.end(), cursor);
    cursor = std::copy(data.begin(), data.end(), cursor);
    std::copy_n(padding_bytes.begin(), padding_size, cursor);

    auto ctx = nxt::crypto::aes256_context{key};
    nxt::crypto::aes256_ige_encrypt(ctx, iv, output, output);
}

inline std::span<std::byte> decrypt_data_with_hash(
    std::span<const std::byte> ciphertext,
    std::span<const std::byte> key,
    std::span<const std::byte> iv,
    std::span<std::byte> output)
{
    if (ciphertext.empty()
        || ciphertext.size() % nxt::crypto::aes_block_len != 0)
        throw protocol_error{"invalid data with hash ciphertext"};
    if (ciphertext.size() < nxt::crypto::sha1_len)
        throw protocol_error{"invalid data with hash ciphertext"};
    if (output.size() != ciphertext.size())
        throw protocol_error{"decrypted data output has the wrong length"};

    auto ctx = nxt::crypto::aes256_context{key};
    nxt::crypto::aes256_ige_decrypt(ctx, iv, ciphertext, output);
    auto hash = std::span<const std::byte>{output}.first(nxt::crypto::sha1_len);
    auto rest = std::span<std::byte>{output}.subspan(nxt::crypto::sha1_len);
    for (auto padding_size = std::size_t{0}; padding_size <= 15; padding_size++) {
        if (rest.size() < padding_size)
            continue;
        auto data = rest.first(rest.size() - padding_size);
        if (std::ranges::equal(hash, nxt::crypto::sha1(data)))
            return data;
    }
    throw protocol_error{"invalid data with hash"};
}

inline std::int64_t server_salt(
    std::span<const std::byte> new_nonce,
    std::span<const std::byte> server_nonce)
{
    if (new_nonce.size() != 32 || server_nonce.size() != 16)
        throw protocol_error{"invalid nonce"};

    auto value = std::uint64_t{0};
    for (auto i = std::size_t{0}; i < 8; i++) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(
                         new_nonce[i] ^ server_nonce[i]))
                 << (i * 8);
    }
    return std::bit_cast<std::int64_t>(value);
}

} // namespace nxt::mt
