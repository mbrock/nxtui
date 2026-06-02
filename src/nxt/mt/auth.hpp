#pragma once

#include <nxt/mt/crypto.hpp>
#include <nxt/mt/tl.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>

namespace nxt::mt::auth {

inline constexpr std::uint32_t req_pq_multi_constructor = 0xbe7e8ef1;
inline constexpr std::uint32_t res_pq_constructor = 0x05162463;
inline constexpr std::uint32_t p_q_inner_data_constructor = 0x83c95aec;
inline constexpr std::uint32_t p_q_inner_data_dc_constructor = 0xa9f55f95;
inline constexpr std::uint32_t req_dh_params_constructor = 0xd712e4be;
inline constexpr std::uint32_t server_dh_params_fail_constructor = 0x79cb045d;
inline constexpr std::uint32_t server_dh_params_ok_constructor = 0xd0e8075c;
inline constexpr std::uint32_t server_dh_inner_data_constructor = 0xb5890dba;
inline constexpr std::uint32_t client_dh_inner_data_constructor = 0x6643b654;
inline constexpr std::uint32_t set_client_dh_params_constructor = 0xf5045f1f;
inline constexpr std::uint32_t dh_gen_ok_constructor = 0x3bcbf734;
inline constexpr std::uint32_t dh_gen_retry_constructor = 0x46dc1fb9;
inline constexpr std::uint32_t dh_gen_fail_constructor = 0xa69dae02;

inline std::size_t unsigned_be_size(unsigned value) noexcept
{
    auto size = std::size_t{1};
    while ((value >>= 8) != 0)
        size++;
    return size;
}

inline void write_unsigned_be(
    std::span<std::byte> out,
    unsigned value)
{
    if (out.size() != unsigned_be_size(value))
        throw protocol_error{"invalid unsigned integer output size"};
    for (auto i = std::size_t{0}; i < out.size(); i++) {
        auto shift = static_cast<unsigned>((out.size() - 1 - i) * 8);
        out[i] = static_cast<std::byte>(value >> shift);
    }
}

inline std::size_t public_key_fingerprint_size(
    std::span<const std::byte> modulus,
    unsigned exponent) noexcept
{
    return tl::bytes_size(modulus.size())
        + tl::bytes_size(unsigned_be_size(exponent));
}

inline std::uint64_t read_u64_le(std::span<const std::byte> input)
{
    if (input.size() != 8)
        throw protocol_error{"invalid little-endian uint64"};
    auto value = std::uint64_t{0};
    for (auto i = std::size_t{0}; i < input.size(); i++) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(input[i]))
                 << (8 * i);
    }
    return value;
}

inline std::uint64_t public_key_fingerprint(
    std::span<const std::byte> modulus,
    unsigned exponent,
    std::span<std::byte> scratch)
{
    auto exponent_bytes = std::array<std::byte, sizeof(unsigned)>{};
    auto exponent_size = unsigned_be_size(exponent);
    auto exponent_span = std::span{exponent_bytes}.first(exponent_size);
    write_unsigned_be(exponent_span, exponent);

    auto needed = public_key_fingerprint_size(modulus, exponent);
    if (scratch.size() < needed)
        throw protocol_error{"public key fingerprint scratch is too small"};
    auto writer = byte_writer{scratch.first(needed)};
    tl::write_bytes(writer, modulus);
    tl::write_bytes(writer, exponent_span);

    auto digest = nxt::crypto::sha1(writer.written());
    return read_u64_le(std::span<const std::byte>{digest}.subspan(12, 8));
}

inline public_key make_public_key(
    std::span<const std::byte> modulus,
    unsigned exponent,
    std::span<std::byte> scratch)
{
    return public_key{
        .modulus = modulus,
        .exponent = exponent,
        .fingerprint = public_key_fingerprint(modulus, exponent, scratch),
    };
}

struct res_pq_view
{
    std::span<const std::byte> nonce;
    std::span<const std::byte> server_nonce;
    std::span<const std::byte> pq;
    std::span<const std::uint64_t> server_public_key_fingerprints;
};

inline std::size_t req_pq_multi_size() noexcept
{
    return 4 + 16;
}

inline void write_req_pq_multi(
    byte_writer & out,
    std::span<const std::byte> nonce)
{
    if (nonce.size() != 16)
        throw protocol_error{"invalid nonce"};
    tl::write_int(out, static_cast<std::int32_t>(req_pq_multi_constructor));
    tl::write_int128(out, nonce);
}

inline res_pq_view decode_res_pq(
    std::span<const std::byte> input,
    std::span<std::uint64_t> fingerprint_storage)
{
    auto reader = tl::reader{input};
    auto constructor = static_cast<std::uint32_t>(reader.int_());
    if (constructor != res_pq_constructor)
        throw protocol_error{"unexpected resPQ constructor"};

    auto nonce = reader.int128();
    auto server_nonce = reader.int128();
    auto pq = reader.bytes();

    auto vector_constructor = static_cast<std::uint32_t>(reader.int_());
    if (vector_constructor != tl::vector_constructor)
        throw protocol_error{"expected vector constructor"};
    auto count = reader.int_();
    if (count < 0)
        throw protocol_error{"negative vector size"};
    auto size = static_cast<std::size_t>(count);
    if (size > fingerprint_storage.size())
        throw protocol_error{"fingerprint storage is too small"};
    for (auto i = std::size_t{0}; i < size; i++)
        fingerprint_storage[i] = reader.long_();
    if (!reader.empty())
        throw protocol_error{"trailing resPQ bytes"};

    return res_pq_view{
        .nonce = nonce,
        .server_nonce = server_nonce,
        .pq = pq,
        .server_public_key_fingerprints = fingerprint_storage.first(size),
    };
}

inline std::uint64_t decode_unsigned_u64(std::span<const std::byte> input)
{
    if (input.empty() || input.size() > 8)
        throw protocol_error{"invalid uint64 bytes"};
    auto value = std::uint64_t{0};
    for (auto byte : input)
        value = (value << 8) | std::to_integer<std::uint8_t>(byte);
    return value;
}

inline std::pair<std::uint64_t, std::uint64_t>
factor_pq(std::span<const std::byte> pq_bytes)
{
    auto value = decode_unsigned_u64(pq_bytes);
    if (value <= 3)
        throw protocol_error{"invalid pq"};
    if (value % 2 == 0)
        return {2, value / 2};

    for (auto c = std::uint64_t{1}; c <= 32; c++) {
        auto x = std::uint64_t{2};
        auto y = std::uint64_t{2};
        for (auto steps = 0; steps < 250'000; steps++) {
            auto step = [&](std::uint64_t v) {
                return (static_cast<unsigned __int128>(v) * v + c) % value;
            };
            x = step(x);
            y = step(step(y));
            auto diff = x > y ? x - y : y - x;
            auto divisor = std::gcd(diff, value);
            if (divisor == 1)
                continue;
            if (divisor == value)
                break;
            auto other = value / divisor;
            if (divisor > other)
                std::swap(divisor, other);
            return {divisor, other};
        }
    }
    throw protocol_error{"factorization failed"};
}

inline std::size_t p_q_inner_data_size(
    std::span<const std::byte> pq,
    std::span<const std::byte> p,
    std::span<const std::byte> q,
    bool with_dc_id = false) noexcept
{
    return 4 + tl::bytes_size(pq.size()) + tl::bytes_size(p.size())
        + tl::bytes_size(q.size()) + 16 + 16 + 32 + (with_dc_id ? 4 : 0);
}

inline void write_p_q_inner_data(
    byte_writer & out,
    std::uint32_t constructor,
    std::span<const std::byte> pq,
    std::span<const std::byte> p,
    std::span<const std::byte> q,
    std::span<const std::byte> nonce,
    std::span<const std::byte> server_nonce,
    std::span<const std::byte> new_nonce,
    std::optional<std::int32_t> dc_id = std::nullopt)
{
    if (constructor != p_q_inner_data_constructor
        && constructor != p_q_inner_data_dc_constructor)
        throw protocol_error{"invalid p_q_inner_data constructor"};
    tl::write_int(out, static_cast<std::int32_t>(constructor));
    tl::write_bytes(out, pq);
    tl::write_bytes(out, p);
    tl::write_bytes(out, q);
    tl::write_int128(out, nonce);
    tl::write_int128(out, server_nonce);
    tl::write_int256(out, new_nonce);
    if (dc_id)
        tl::write_int(out, *dc_id);
}

inline std::size_t req_dh_params_size(
    std::span<const std::byte> p,
    std::span<const std::byte> q,
    std::span<const std::byte> encrypted_data) noexcept
{
    return 4 + 16 + 16 + tl::bytes_size(p.size()) + tl::bytes_size(q.size())
        + 8 + tl::bytes_size(encrypted_data.size());
}

inline void write_req_dh_params(
    byte_writer & out,
    std::span<const std::byte> nonce,
    std::span<const std::byte> server_nonce,
    std::span<const std::byte> p,
    std::span<const std::byte> q,
    std::uint64_t fingerprint,
    std::span<const std::byte> encrypted_data)
{
    tl::write_int(out, static_cast<std::int32_t>(req_dh_params_constructor));
    tl::write_int128(out, nonce);
    tl::write_int128(out, server_nonce);
    tl::write_bytes(out, p);
    tl::write_bytes(out, q);
    tl::write_long(out, fingerprint);
    tl::write_bytes(out, encrypted_data);
}

inline const public_key * select_public_key(
    std::span<const public_key> keys,
    std::span<const std::uint64_t> fingerprints) noexcept
{
    for (auto fingerprint : fingerprints) {
        auto found = std::ranges::find_if(keys, [&](const public_key & key) {
            return key.fingerprint == fingerprint;
        });
        if (found != keys.end())
            return &*found;
    }
    return nullptr;
}

} // namespace nxt::mt::auth
