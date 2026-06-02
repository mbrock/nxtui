#include <nxt/crypto.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace nxt::crypto {
namespace {

using u8 = std::uint8_t;
using block = std::array<std::byte, 16>;

constexpr auto sbox = std::array<u8, 256>{
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

constexpr auto inv_sbox = [] {
    auto out = std::array<u8, 256>{};
    for (auto i = std::size_t{0}; i < sbox.size(); i++)
        out[sbox[i]] = static_cast<u8>(i);
    return out;
}();

u8 xtime(u8 value)
{
    return static_cast<u8>((value << 1) ^ ((value >> 7) * 0x1b));
}

u8 gmul(u8 a, u8 b)
{
    auto out = u8{0};
    for (auto i = 0; i < 8; i++) {
        if ((b & 1) != 0)
            out ^= a;
        auto high = a & 0x80;
        a = static_cast<u8>(a << 1);
        if (high != 0)
            a ^= 0x1b;
        b >>= 1;
    }
    return out;
}

void require_size(
    std::span<const std::byte> value,
    std::size_t size,
    const char * message)
{
    if (value.size() != size)
        throw crypto_error{message};
}

void require_initialized(bool initialized, const char * message)
{
    if (!initialized)
        throw crypto_error{message};
}

u8 byte(std::byte value)
{
    return std::to_integer<u8>(value);
}

void add_round_key(
    block & state,
    std::span<const std::byte> round_keys,
    int round)
{
    auto key = round_keys.subspan(static_cast<std::size_t>(round) * 16, 16);
    for (auto i = 0; i < 16; i++)
        state[i] ^= key[i];
}

void shift_rows(block & state)
{
    constexpr auto sr = std::array<int, 16>{
        0,5,10,15,
        4,9,14,3,
        8,13,2,7,
        12,1,6,11,
    };
    auto out = block{};
    for (auto i = 0; i < 16; i++)
        out[i] = state[sr[i]];
    state = out;
}

void inv_shift_rows(block & state)
{
    constexpr auto sr = std::array<int, 16>{
        0,13,10,7,
        4,1,14,11,
        8,5,2,15,
        12,9,6,3,
    };
    auto out = block{};
    for (auto i = 0; i < 16; i++)
        out[i] = state[sr[i]];
    state = out;
}

void mix_columns(block & state)
{
    for (auto c = 0; c < 4; c++) {
        auto * col = state.data() + c * 4;
        auto a0 = byte(col[0]);
        auto a1 = byte(col[1]);
        auto a2 = byte(col[2]);
        auto a3 = byte(col[3]);
        col[0] = static_cast<std::byte>(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        col[1] = static_cast<std::byte>(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        col[2] = static_cast<std::byte>(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        col[3] = static_cast<std::byte>(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
    }
}

void inv_mix_columns(block & state)
{
    for (auto c = 0; c < 4; c++) {
        auto * col = state.data() + c * 4;
        auto a0 = byte(col[0]);
        auto a1 = byte(col[1]);
        auto a2 = byte(col[2]);
        auto a3 = byte(col[3]);
        col[0] = static_cast<std::byte>(
            gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        col[1] = static_cast<std::byte>(
            gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        col[2] = static_cast<std::byte>(
            gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        col[3] = static_cast<std::byte>(
            gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}

void aes_encrypt_block(
    std::span<const std::byte> round_keys,
    int rounds,
    block const & in,
    block & out)
{
    auto s = in;
    add_round_key(s, round_keys, 0);

    for (auto round = 1; round <= rounds; round++) {
        for (auto & b : s)
            b = static_cast<std::byte>(sbox[byte(b)]);

        shift_rows(s);

        if (round < rounds)
            mix_columns(s);

        add_round_key(s, round_keys, round);
    }
    out = s;
}

void aes_decrypt_block(
    std::span<const std::byte> round_keys,
    int rounds,
    block const & in,
    block & out)
{
    auto s = in;
    add_round_key(s, round_keys, rounds);

    for (auto round = rounds - 1; round >= 1; round--) {
        inv_shift_rows(s);
        for (auto & b : s)
            b = static_cast<std::byte>(inv_sbox[byte(b)]);
        add_round_key(s, round_keys, round);
        inv_mix_columns(s);
    }

    inv_shift_rows(s);
    for (auto & b : s)
        b = static_cast<std::byte>(inv_sbox[byte(b)]);
    add_round_key(s, round_keys, 0);
    out = s;
}

void expand_aes_key(
    std::span<const std::byte> key,
    std::span<std::byte> round_keys)
{
    std::ranges::copy(key, round_keys.begin());
    auto generated = key.size();
    auto rcon = u8{1};
    auto temp = std::array<std::byte, 4>{};
    while (generated < round_keys.size()) {
        std::ranges::copy(
            round_keys.subspan(generated - 4, 4),
            temp.begin());
        if (generated % key.size() == 0) {
            temp = {
                static_cast<std::byte>(sbox[byte(temp[1])] ^ rcon),
                static_cast<std::byte>(sbox[byte(temp[2])]),
                static_cast<std::byte>(sbox[byte(temp[3])]),
                static_cast<std::byte>(sbox[byte(temp[0])]),
            };
            rcon = xtime(rcon);
        } else if (key.size() > 24 && generated % key.size() == 16) {
            for (auto & b : temp)
                b = static_cast<std::byte>(sbox[byte(b)]);
        }

        for (auto i = 0; i < 4; i++) {
            round_keys[generated] =
                round_keys[generated - key.size()] ^ temp[i];
            generated++;
        }
    }
}

block gf_mul(block x, block v)
{
    auto z = block{};
    for (auto i = 0; i < 128; i++) {
        if ((byte(x[i >> 3]) & (0x80 >> (i & 7))) != 0)
            for (auto j = 0; j < 16; j++)
                z[j] ^= v[j];

        auto lsb = byte(v[15]) & 1;
        for (auto j = 15; j > 0; j--)
            v[j] = static_cast<std::byte>((byte(v[j]) >> 1) | (byte(v[j - 1]) << 7));
        v[0] = static_cast<std::byte>(byte(v[0]) >> 1);
        if (lsb != 0)
            v[0] ^= std::byte{0xe1};
    }
    return z;
}

void ghash_block(block const & h, block & acc, block const & data)
{
    for (auto i = 0; i < 16; i++)
        acc[i] ^= data[i];
    acc = gf_mul(acc, h);
}

void ghash_update(block const & h, block & acc, std::span<const std::byte> data)
{
    while (data.size() >= 16) {
        auto b = block{};
        std::ranges::copy(data.first(16), b.begin());
        ghash_block(h, acc, b);
        data = data.subspan(16);
    }
    if (!data.empty()) {
        auto b = block{};
        std::ranges::copy(data, b.begin());
        ghash_block(h, acc, b);
    }
}

void put_be64(block & out, std::size_t offset, std::uint64_t value)
{
    for (auto i = 0; i < 8; i++)
        out[offset + i] = static_cast<std::byte>(value >> (56 - 8 * i));
}

block initial_counter(std::span<const std::byte> nonce)
{
    auto out = block{};
    std::ranges::copy(nonce, out.begin());
    out[15] = std::byte{1};
    return out;
}

void increment32(block & ctr)
{
    for (auto i = 15; i >= 12; i--) {
        ctr[i] = static_cast<std::byte>(byte(ctr[i]) + 1);
        if (ctr[i] != std::byte{0})
            break;
    }
}

block ghash_finish(
    aes128gcm_context const & ctx,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext)
{
    auto acc = block{};
    ghash_update(ctx.ghash_key_, acc, aad);
    ghash_update(ctx.ghash_key_, acc, ciphertext);
    auto len = block{};
    put_be64(len, 0, static_cast<std::uint64_t>(aad.size()) * 8);
    put_be64(len, 8, static_cast<std::uint64_t>(ciphertext.size()) * 8);
    ghash_block(ctx.ghash_key_, acc, len);
    return acc;
}

void crypt_ctr(
    aes128gcm_context const & ctx,
    block counter,
    std::span<const std::byte> input,
    std::span<std::byte> output)
{
    auto stream = block{};
    auto offset = std::size_t{0};
    while (offset != input.size()) {
        increment32(counter);
        aes_encrypt_block(ctx.round_keys_, 10, counter, stream);
        auto n = std::min(std::size_t{16}, input.size() - offset);
        for (auto i = std::size_t{0}; i < n; i++)
            output[offset + i] = input[offset + i] ^ stream[i];
        offset += n;
    }
}

bool tag_equal(block const & a, std::span<const std::byte> b)
{
    auto diff = std::byte{0};
    for (auto i = 0; i < 16; i++)
        diff |= a[i] ^ b[i];
    return diff == std::byte{0};
}

} // namespace

aes128gcm_context::aes128gcm_context(std::span<const std::byte> key)
{
    require_size(key, aes128_key_len, "AES-128-GCM key must be 16 bytes");
    expand_aes_key(key, round_keys_);

    auto zero = block{};
    aes_encrypt_block(round_keys_, 10, zero, ghash_key_);
    initialized_ = true;
}

aes256_context::aes256_context(std::span<const std::byte> key)
{
    require_size(key, aes256_key_len, "AES-256 key must be 32 bytes");
    expand_aes_key(key, round_keys_);
    initialized_ = true;
}

std::optional<bytes> aes128gcm_open_impl(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext)
{
    require_initialized(
        ctx.initialized_,
        "AES-128-GCM context is not initialized");
    require_size(nonce, aes_gcm_nonce_len, "AES-GCM nonce must be 12 bytes");
    if (ciphertext.size() < aes_gcm_tag_len)
        return std::nullopt;

    auto text = ciphertext.first(ciphertext.size() - aes_gcm_tag_len);
    auto tag = ciphertext.last(aes_gcm_tag_len);
    auto acc = ghash_finish(ctx, aad, text);
    auto s = block{};
    auto j0 = initial_counter(nonce);
    aes_encrypt_block(ctx.round_keys_, 10, j0, s);
    for (auto i = 0; i < 16; i++)
        acc[i] ^= s[i];
    if (!tag_equal(acc, tag))
        return std::nullopt;

    auto out = bytes(text.size());
    crypt_ctr(ctx, j0, text, out);
    return out;
}

std::optional<std::span<std::byte>> aes128gcm_open_in_place_impl(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<std::byte> ciphertext)
{
    require_initialized(
        ctx.initialized_,
        "AES-128-GCM context is not initialized");
    require_size(nonce, aes_gcm_nonce_len, "AES-GCM nonce must be 12 bytes");
    if (ciphertext.size() < aes_gcm_tag_len)
        return std::nullopt;

    auto text = ciphertext.first(ciphertext.size() - aes_gcm_tag_len);
    auto tag = std::span<const std::byte>{ciphertext}.last(aes_gcm_tag_len);
    auto acc = ghash_finish(ctx, aad, text);
    auto s = block{};
    auto j0 = initial_counter(nonce);
    aes_encrypt_block(ctx.round_keys_, 10, j0, s);
    for (auto i = 0; i < 16; i++)
        acc[i] ^= s[i];
    if (!tag_equal(acc, tag))
        return std::nullopt;

    crypt_ctr(ctx, j0, text, text);
    return text;
}

bytes aes128gcm_seal_impl(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> plaintext)
{
    require_initialized(
        ctx.initialized_,
        "AES-128-GCM context is not initialized");
    require_size(nonce, aes_gcm_nonce_len, "AES-GCM nonce must be 12 bytes");

    auto out = bytes(plaintext.size() + aes_gcm_tag_len);
    auto text = std::span{out}.first(plaintext.size());
    auto j0 = initial_counter(nonce);
    crypt_ctr(ctx, j0, plaintext, text);

    auto tag = ghash_finish(ctx, aad, text);
    auto s = block{};
    aes_encrypt_block(ctx.round_keys_, 10, j0, s);
    for (auto i = 0; i < 16; i++)
        out[plaintext.size() + i] = tag[i] ^ s[i];
    return out;
}

void aes256_encrypt_block(
    const aes256_context & ctx,
    std::span<const std::byte> input,
    std::span<std::byte> output)
{
    require_initialized(ctx.initialized_, "AES-256 context is not initialized");
    require_size(input, aes_block_len, "AES block input must be 16 bytes");
    require_size(output, aes_block_len, "AES block output must be 16 bytes");

    auto in = block{};
    auto out = block{};
    std::ranges::copy(input, in.begin());
    aes_encrypt_block(ctx.round_keys_, 14, in, out);
    std::ranges::copy(out, output.begin());
}

void aes256_decrypt_block(
    const aes256_context & ctx,
    std::span<const std::byte> input,
    std::span<std::byte> output)
{
    require_initialized(ctx.initialized_, "AES-256 context is not initialized");
    require_size(input, aes_block_len, "AES block input must be 16 bytes");
    require_size(output, aes_block_len, "AES block output must be 16 bytes");

    auto in = block{};
    auto out = block{};
    std::ranges::copy(input, in.begin());
    aes_decrypt_block(ctx.round_keys_, 14, in, out);
    std::ranges::copy(out, output.begin());
}

void require_ige_spans(
    std::span<const std::byte> iv,
    std::span<const std::byte> input,
    std::span<std::byte> output)
{
    require_size(iv, aes_ige_iv_len, "AES-IGE IV must be 32 bytes");
    if (input.size() % aes_block_len != 0)
        throw crypto_error{"AES-IGE input must be a multiple of 16 bytes"};
    if (output.size() != input.size())
        throw crypto_error{"AES-IGE output must match input size"};
}

void aes256_ige_encrypt(
    const aes256_context & ctx,
    std::span<const std::byte> iv,
    std::span<const std::byte> input,
    std::span<std::byte> output)
{
    require_initialized(ctx.initialized_, "AES-256 context is not initialized");
    require_ige_spans(iv, input, output);

    auto c_prev = block{};
    auto p_prev = block{};
    std::ranges::copy(iv.first(16), c_prev.begin());
    std::ranges::copy(iv.subspan(16), p_prev.begin());

    auto block_in = block{};
    auto block_out = block{};
    for (auto offset = std::size_t{0}; offset < input.size(); offset += 16) {
        std::ranges::copy(input.subspan(offset, 16), block_in.begin());
        for (auto i = 0; i < 16; i++)
            block_out[i] = block_in[i] ^ c_prev[i];
        aes_encrypt_block(ctx.round_keys_, 14, block_out, block_out);
        for (auto i = 0; i < 16; i++)
            block_out[i] ^= p_prev[i];
        std::ranges::copy(block_out, output.begin() + offset);
        p_prev = block_in;
        c_prev = block_out;
    }
}

void aes256_ige_decrypt(
    const aes256_context & ctx,
    std::span<const std::byte> iv,
    std::span<const std::byte> input,
    std::span<std::byte> output)
{
    require_initialized(ctx.initialized_, "AES-256 context is not initialized");
    require_ige_spans(iv, input, output);

    auto c_prev = block{};
    auto p_prev = block{};
    std::ranges::copy(iv.first(16), c_prev.begin());
    std::ranges::copy(iv.subspan(16), p_prev.begin());

    auto block_in = block{};
    auto block_out = block{};
    for (auto offset = std::size_t{0}; offset < input.size(); offset += 16) {
        std::ranges::copy(input.subspan(offset, 16), block_in.begin());
        for (auto i = 0; i < 16; i++)
            block_out[i] = block_in[i] ^ p_prev[i];
        aes_decrypt_block(ctx.round_keys_, 14, block_out, block_out);
        for (auto i = 0; i < 16; i++)
            block_out[i] ^= c_prev[i];
        std::ranges::copy(block_out, output.begin() + offset);
        c_prev = block_in;
        p_prev = block_out;
    }
}

} // namespace nxt::crypto
