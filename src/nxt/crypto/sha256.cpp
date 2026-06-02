#include <nxt/crypto.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace nxt::crypto {
namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr auto sha256_k = std::array<u32, 64>{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

u32 sha256_load32_be(const std::byte * bytes)
{
    return (u32{static_cast<unsigned char>(bytes[0])} << 24)
         | (u32{static_cast<unsigned char>(bytes[1])} << 16)
         | (u32{static_cast<unsigned char>(bytes[2])} << 8)
         | u32{static_cast<unsigned char>(bytes[3])};
}

void sha256_store32_be(std::byte * out, u32 value)
{
    out[0] = static_cast<std::byte>(value >> 24);
    out[1] = static_cast<std::byte>(value >> 16);
    out[2] = static_cast<std::byte>(value >> 8);
    out[3] = static_cast<std::byte>(value);
}

void sha256_compress(std::array<u32, 8> & state, const std::byte block[64])
{
    auto w = std::array<u32, 64>{};
    for (auto i = 0; i < 16; i++)
        w[i] = sha256_load32_be(block + 4 * i);
    for (auto i = 16; i < 64; i++) {
        const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18)
                      ^ (w[i - 15] >> 3);
        const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19)
                      ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];

    for (auto i = 0; i < 64; i++) {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto ch = (e & f) ^ (~e & g);
        const auto temp1 = h + s1 + ch + sha256_k[i] + w[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto maj = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}

sha256_state::sha256_state()
    : state_{
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19,
    }
{}

void sha256_state::update(std::span<const std::byte> input)
{
    size_ += input.size();

    if (buffered_ != 0) {
        const auto take = std::min(input.size(), buffer_.size() - buffered_);
        std::ranges::copy(input.first(take), buffer_.begin() + buffered_);
        buffered_ += take;
        input = input.subspan(take);
        if (buffered_ == buffer_.size()) {
            sha256_compress(state_, buffer_.data());
            buffered_ = 0;
        }
    }

    while (input.size() >= buffer_.size()) {
        sha256_compress(state_, input.data());
        input = input.subspan(buffer_.size());
    }

    if (!input.empty()) {
        std::ranges::copy(input, buffer_.begin());
        buffered_ = input.size();
    }
}

std::array<std::byte, sha256_len> sha256_state::finalize() const
{
    auto state = state_;
    auto block = std::array<std::byte, 128>{};
    std::ranges::copy(std::span{buffer_}.first(buffered_), block.begin());
    block[buffered_] = std::byte{0x80};

    const auto final_size =
        buffered_ < 56 ? std::size_t{64} : std::size_t{128};
    const auto bit_size = size_ * 8;
    for (auto i = 0; i < 8; i++)
        block[final_size - 1 - i] = static_cast<std::byte>(bit_size >> (8 * i));

    sha256_compress(state, block.data());
    if (final_size == 128)
        sha256_compress(state, block.data() + 64);

    auto out = std::array<std::byte, sha256_len>{};
    for (auto i = 0; i < 8; i++)
        sha256_store32_be(out.data() + 4 * i, state[i]);
    return out;
}

std::array<std::byte, sha256_len> sha256(std::span<const std::byte> input)
{
    auto state = sha256_state{};
    state.update(input);
    return state.finalize();
}

namespace {

std::array<std::byte, sha256_block_len>
hmac_key_block(std::span<const std::byte> key)
{
    auto out = std::array<std::byte, sha256_block_len>{};
    if (key.size() > sha256_block_len) {
        auto digest = sha256(key);
        std::ranges::copy(digest, out.begin());
    } else {
        std::ranges::copy(key, out.begin());
    }
    return out;
}

}

hmac_sha256_state::hmac_sha256_state(std::span<const std::byte> key)
{
    auto key_block = hmac_key_block(key);
    auto inner_pad = std::array<std::byte, sha256_block_len>{};
    for (auto i = std::size_t{0}; i < sha256_block_len; i++) {
        inner_pad[i] = key_block[i] ^ std::byte{0x36};
        outer_pad_[i] = key_block[i] ^ std::byte{0x5c};
    }
    inner_.update(inner_pad);
}

void hmac_sha256_state::update(std::span<const std::byte> input)
{
    inner_.update(input);
}

std::array<std::byte, sha256_len> hmac_sha256_state::finalize() const
{
    auto outer = sha256_state{};
    outer.update(outer_pad_);
    outer.update(inner_.finalize());
    return outer.finalize();
}

std::array<std::byte, sha256_len> hmac_sha256(
    std::span<const std::byte> key,
    std::span<const std::byte> data)
{
    auto state = hmac_sha256_state{key};
    state.update(data);
    return state.finalize();
}

} // namespace nxt::crypto
