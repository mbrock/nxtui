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
    std::array<std::byte, 32> temp_aes_key{};
    std::array<std::byte, 32> temp_aes_iv{};
    auth_key key{};
    std::size_t p_size = 0;
    std::size_t q_size = 0;
    std::uint64_t selected_public_key_fingerprint = 0;
    std::int64_t server_salt = 0;
    std::int64_t time_offset = 0;
    bool has_key = false;

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

struct server_dh_inner_data_view
{
    std::span<const std::byte> nonce;
    std::span<const std::byte> server_nonce;
    std::int32_t g = 0;
    std::span<const std::byte> dh_prime;
    std::span<const std::byte> g_a;
    std::int32_t server_time = 0;
};

struct dh_gen_response_view
{
    std::uint32_t constructor = 0;
    std::span<const std::byte> nonce;
    std::span<const std::byte> server_nonce;
    std::span<const std::byte> new_nonce_hash;
    std::uint8_t hash_number = 0;
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

inline server_dh_inner_data_view decode_server_dh_inner_data(
    std::span<const std::byte> input)
{
    auto reader = tl::reader{input};
    auto constructor = static_cast<std::uint32_t>(reader.int_());
    if (constructor != server_dh_inner_data_constructor)
        throw protocol_error{"unexpected server_DH_inner_data constructor"};
    auto out = server_dh_inner_data_view{
        .nonce = reader.int128(),
        .server_nonce = reader.int128(),
        .g = reader.int_(),
        .dh_prime = reader.bytes(),
        .g_a = reader.bytes(),
        .server_time = reader.int_(),
    };
    if (!reader.empty())
        throw protocol_error{"trailing server_DH_inner_data"};
    return out;
}

inline std::size_t client_dh_inner_data_size(
    std::span<const std::byte> g_b) noexcept
{
    return 4 + 16 + 16 + 8 + tl::bytes_size(g_b.size());
}

inline void write_client_dh_inner_data(
    byte_writer & out,
    std::span<const std::byte> nonce,
    std::span<const std::byte> server_nonce,
    std::uint64_t retry_id,
    std::span<const std::byte> g_b)
{
    tl::write_int(out, static_cast<std::int32_t>(client_dh_inner_data_constructor));
    tl::write_int128(out, nonce);
    tl::write_int128(out, server_nonce);
    tl::write_long(out, retry_id);
    tl::write_bytes(out, g_b);
}

inline std::size_t set_client_dh_params_size(
    std::span<const std::byte> encrypted_data) noexcept
{
    return 4 + 16 + 16 + tl::bytes_size(encrypted_data.size());
}

inline void write_set_client_dh_params(
    byte_writer & out,
    std::span<const std::byte> nonce,
    std::span<const std::byte> server_nonce,
    std::span<const std::byte> encrypted_data)
{
    tl::write_int(out, static_cast<std::int32_t>(set_client_dh_params_constructor));
    tl::write_int128(out, nonce);
    tl::write_int128(out, server_nonce);
    tl::write_bytes(out, encrypted_data);
}

inline dh_gen_response_view decode_dh_gen_response(
    std::span<const std::byte> body)
{
    auto reader = tl::reader{body};
    auto constructor = static_cast<std::uint32_t>(reader.int_());
    auto hash_number = std::uint8_t{0};
    if (constructor == dh_gen_ok_constructor)
        hash_number = 1;
    else if (constructor == dh_gen_retry_constructor)
        hash_number = 2;
    else if (constructor == dh_gen_fail_constructor)
        hash_number = 3;
    else
        throw protocol_error{"unexpected dh_gen constructor"};

    auto out = dh_gen_response_view{
        .constructor = constructor,
        .nonce = reader.int128(),
        .server_nonce = reader.int128(),
        .new_nonce_hash = reader.int128(),
        .hash_number = hash_number,
    };
    if (!reader.empty())
        throw protocol_error{"trailing dh_gen answer"};
    return out;
}

inline std::array<std::byte, 16> server_dh_fail_hash(
    std::span<const std::byte> new_nonce)
{
    auto digest = nxt::crypto::sha1(new_nonce);
    auto out = std::array<std::byte, 16>{};
    std::ranges::copy(std::span{digest}.subspan(4, 16), out.begin());
    return out;
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

inline void receive_server_dh_params(
    exchange_state & state,
    std::span<const std::byte> body,
    std::span<const std::byte> random_bytes,
    std::int32_t now,
    std::span<std::byte> decrypted_storage,
    std::span<std::byte> g_b_storage,
    std::span<std::byte> auth_key_storage,
    std::span<std::byte> client_inner_storage,
    std::span<std::byte> encrypted_storage,
    byte_writer & out)
{
    if (state.current_phase != phase::awaiting_server_dh_params)
        throw protocol_error{"unexpected auth phase"};
    if (random_bytes.size() < 256)
        throw protocol_error{"insufficient random bytes"};
    if (auth_key_storage.size() != 256)
        throw protocol_error{"auth key storage must be 256 bytes"};

    auto reader = tl::reader{body};
    auto constructor = static_cast<std::uint32_t>(reader.int_());
    auto nonce = reader.int128();
    auto server_nonce = reader.int128();
    if (!std::ranges::equal(nonce, state.nonce)
        || !std::ranges::equal(server_nonce, state.server_nonce))
        throw protocol_error{"nonce mismatch"};

    if (constructor == server_dh_params_fail_constructor) {
        auto actual_hash = reader.int128();
        if (!reader.empty())
            throw protocol_error{"trailing server_DH_params_fail"};
        if (!std::ranges::equal(actual_hash, server_dh_fail_hash(state.new_nonce)))
            throw protocol_error{"new nonce hash mismatch"};
        throw protocol_error{"server dh params fail"};
    }
    if (constructor != server_dh_params_ok_constructor)
        throw protocol_error{"unexpected server dh params constructor"};

    auto encrypted_answer = reader.bytes();
    if (!reader.empty())
        throw protocol_error{"trailing server_DH_params_ok"};
    if (decrypted_storage.size() < encrypted_answer.size())
        throw protocol_error{"decrypted server_DH_params storage is too small"};

    auto key_iv = temp_aes_key_iv(state.server_nonce, state.new_nonce);
    std::ranges::copy(key_iv.key, state.temp_aes_key.begin());
    std::ranges::copy(key_iv.iv, state.temp_aes_iv.begin());
    auto decrypted = decrypt_data_with_hash(
        encrypted_answer,
        key_iv.key,
        key_iv.iv,
        decrypted_storage.first(encrypted_answer.size()));
    auto inner = decode_server_dh_inner_data(decrypted);
    if (!std::ranges::equal(inner.nonce, state.nonce)
        || !std::ranges::equal(inner.server_nonce, state.server_nonce))
        throw protocol_error{"nonce mismatch"};
    if (g_b_storage.size() < inner.dh_prime.size())
        throw protocol_error{"g_b storage is too small"};

    auto g_bytes_storage = std::array<std::byte, 8>{};
    auto g_bytes = write_unsigned_be(
        g_bytes_storage,
        static_cast<std::uint64_t>(inner.g));
    auto b = random_bytes.first(256);
    auto g_b = g_b_storage.first(inner.dh_prime.size());
    nxt::crypto::modular_exponentiate(
        g_bytes,
        b,
        inner.dh_prime,
        g_b);
    nxt::crypto::modular_exponentiate(
        inner.g_a,
        b,
        inner.dh_prime,
        auth_key_storage);
    state.key = make_auth_key(auth_key_storage);
    state.has_key = true;
    state.server_salt = server_salt(state.new_nonce, state.server_nonce);
    state.time_offset = static_cast<std::int64_t>(inner.server_time) - now;

    auto client_inner_size = client_dh_inner_data_size(g_b);
    if (client_inner_storage.size() < client_inner_size)
        throw protocol_error{"client_DH_inner_data storage is too small"};
    auto client_inner_writer =
        byte_writer{client_inner_storage.first(client_inner_size)};
    write_client_dh_inner_data(
        client_inner_writer,
        state.nonce,
        state.server_nonce,
        0,
        g_b);

    auto encrypted_size =
        encrypted_data_with_hash_size(client_inner_writer.written().size());
    if (encrypted_storage.size() < encrypted_size)
        throw protocol_error{"client encrypted data storage is too small"};
    auto encrypted = encrypted_storage.first(encrypted_size);
    encrypt_data_with_hash(
        client_inner_writer.written(),
        key_iv.key,
        key_iv.iv,
        random_bytes.subspan(256),
        encrypted);
    write_set_client_dh_params(
        out,
        state.nonce,
        state.server_nonce,
        encrypted);
    state.current_phase = phase::awaiting_dh_gen;
}

inline void receive_dh_gen(
    exchange_state & state,
    std::span<const std::byte> body)
{
    if (state.current_phase != phase::awaiting_dh_gen || !state.has_key)
        throw protocol_error{"unexpected auth phase"};

    auto response = decode_dh_gen_response(body);
    if (!std::ranges::equal(response.nonce, state.nonce)
        || !std::ranges::equal(response.server_nonce, state.server_nonce))
        throw protocol_error{"nonce mismatch"};
    if (!std::ranges::equal(
            response.new_nonce_hash,
            calc_new_nonce_hash(
                state.key,
                state.new_nonce,
                response.hash_number)))
        throw protocol_error{"new nonce hash mismatch"};

    if (response.constructor == dh_gen_retry_constructor)
        throw protocol_error{"dh_gen_retry"};
    if (response.constructor == dh_gen_fail_constructor)
        throw protocol_error{"dh_gen_fail"};

    state.current_phase = phase::complete;
}

} // namespace nxt::mt::auth
