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

enum class phase
{
    idle,
    awaiting_res_pq,
    awaiting_server_dh_params,
    awaiting_dh_gen,
    complete,
};

struct exchange_state
{
    phase current_phase = phase::idle;
    std::array<std::byte, 16> nonce{};
    std::array<std::byte, 16> server_nonce{};
    std::array<std::byte, 32> new_nonce{};
    std::array<std::byte, 8> p{};
    std::array<std::byte, 8> q{};
    std::size_t p_size = 0;
    std::size_t q_size = 0;
    std::uint64_t selected_public_key_fingerprint = 0;

    [[nodiscard]] std::span<const std::byte> p_bytes() const noexcept
    {
        return std::span{p}.first(p_size);
    }

    [[nodiscard]] std::span<const std::byte> q_bytes() const noexcept
    {
        return std::span{q}.first(q_size);
    }
};

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

inline std::size_t unsigned_be_size(std::uint64_t value) noexcept
{
    auto size = std::size_t{1};
    while ((value >>= 8) != 0)
        size++;
    return size;
}

inline std::span<const std::byte> write_unsigned_be(
    std::span<std::byte> out,
    std::uint64_t value)
{
    auto size = unsigned_be_size(value);
    if (out.size() < size)
        throw protocol_error{"unsigned integer output is too small"};
    auto encoded = out.first(size);
    for (auto i = std::size_t{0}; i < encoded.size(); i++) {
        auto shift = static_cast<unsigned>((encoded.size() - 1 - i) * 8);
        encoded[i] = static_cast<std::byte>(value >> shift);
    }
    return encoded;
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

inline void begin(
    exchange_state & state,
    std::span<const std::byte> nonce,
    byte_writer & out)
{
    if (nonce.size() != state.nonce.size())
        throw protocol_error{"invalid nonce"};
    state = exchange_state{};
    state.current_phase = phase::awaiting_res_pq;
    std::ranges::copy(nonce, state.nonce.begin());
    write_req_pq_multi(out, state.nonce);
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

inline void receive_res_pq(
    exchange_state & state,
    std::span<const std::byte> response,
    std::span<const public_key> public_keys,
    std::span<const std::byte> random_bytes,
    std::span<std::uint64_t> fingerprint_storage,
    std::span<std::byte> inner_storage,
    std::span<std::byte> encrypted_storage,
    byte_writer & out,
    std::int32_t dc_id = 0,
    bool include_dc_id = false)
{
    if (state.current_phase != phase::awaiting_res_pq)
        throw protocol_error{"unexpected auth phase"};
    if (random_bytes.size() < 32 + rsa_required_random_bytes())
        throw protocol_error{"insufficient random bytes"};

    auto decoded = decode_res_pq(response, fingerprint_storage);
    if (!std::ranges::equal(decoded.nonce, state.nonce))
        throw protocol_error{"nonce mismatch"};
    auto * key = select_public_key(
        public_keys,
        decoded.server_public_key_fingerprints);
    if (key == nullptr)
        throw protocol_error{"unknown public key"};

    auto [p_value, q_value] = factor_pq(decoded.pq);
    auto p = write_unsigned_be(state.p, p_value);
    auto q = write_unsigned_be(state.q, q_value);
    state.p_size = p.size();
    state.q_size = q.size();
    std::ranges::copy(decoded.server_nonce, state.server_nonce.begin());
    std::ranges::copy(random_bytes.first(32), state.new_nonce.begin());

    auto inner_size = p_q_inner_data_size(
        decoded.pq,
        p,
        q,
        include_dc_id);
    if (inner_storage.size() < inner_size)
        throw protocol_error{"p_q_inner_data storage is too small"};
    auto inner_writer = byte_writer{inner_storage.first(inner_size)};
    write_p_q_inner_data(
        inner_writer,
        include_dc_id ? p_q_inner_data_dc_constructor
                      : p_q_inner_data_constructor,
        decoded.pq,
        p,
        q,
        state.nonce,
        state.server_nonce,
        state.new_nonce,
        include_dc_id ? std::optional<std::int32_t>{dc_id} : std::nullopt);

    if (encrypted_storage.size() < rsa_padded_block_size)
        throw protocol_error{"encrypted data storage is too small"};
    auto encrypted_data = encrypted_storage.first(rsa_padded_block_size);
    rsa_encrypt_padded(
        inner_writer.written(),
        *key,
        random_bytes.subspan(32, rsa_required_random_bytes()),
        encrypted_data);

    write_req_dh_params(
        out,
        state.nonce,
        state.server_nonce,
        p,
        q,
        key->fingerprint,
        encrypted_data);

    state.current_phase = phase::awaiting_server_dh_params;
    state.selected_public_key_fingerprint = key->fingerprint;
}

} // namespace nxt::mt::auth
