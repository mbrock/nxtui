#include <nxt/crypto.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace nxt::crypto {
namespace {

using u32 = std::uint32_t;

u32 load32_be(const std::byte * bytes)
{
    return (u32{static_cast<unsigned char>(bytes[0])} << 24)
         | (u32{static_cast<unsigned char>(bytes[1])} << 16)
         | (u32{static_cast<unsigned char>(bytes[2])} << 8)
         | u32{static_cast<unsigned char>(bytes[3])};
}

void store32_be(std::byte * out, u32 value)
{
    out[0] = static_cast<std::byte>(value >> 24);
    out[1] = static_cast<std::byte>(value >> 16);
    out[2] = static_cast<std::byte>(value >> 8);
    out[3] = static_cast<std::byte>(value);
}

void compress(std::array<u32, 5> & state, const std::byte block[64])
{
    auto w = std::array<u32, 80>{};
    for (auto i = 0; i < 16; i++)
        w[i] = load32_be(block + 4 * i);
    for (auto i = 16; i < 80; i++)
        w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];

    for (auto i = 0; i < 80; i++) {
        auto f = u32{};
        auto k = u32{};
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }

        auto temp = std::rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = std::rotl(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

} // namespace

sha1_state::sha1_state()
    : state_{
        0x67452301,
        0xefcdab89,
        0x98badcfe,
        0x10325476,
        0xc3d2e1f0,
    }
{}

void sha1_state::update(std::span<const std::byte> input)
{
    size_ += input.size();

    if (buffered_ != 0) {
        const auto take = std::min(input.size(), buffer_.size() - buffered_);
        std::ranges::copy(input.first(take), buffer_.begin() + buffered_);
        buffered_ += take;
        input = input.subspan(take);
        if (buffered_ == buffer_.size()) {
            compress(state_, buffer_.data());
            buffered_ = 0;
        }
    }

    while (input.size() >= buffer_.size()) {
        compress(state_, input.data());
        input = input.subspan(buffer_.size());
    }

    if (!input.empty()) {
        std::ranges::copy(input, buffer_.begin());
        buffered_ = input.size();
    }
}

std::array<std::byte, sha1_len> sha1_state::finalize() const
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

    compress(state, block.data());
    if (final_size == 128)
        compress(state, block.data() + 64);

    auto out = std::array<std::byte, sha1_len>{};
    for (auto i = 0; i < 5; i++)
        store32_be(out.data() + 4 * i, state[i]);
    return out;
}

std::array<std::byte, sha1_len> sha1(std::span<const std::byte> input)
{
    auto state = sha1_state{};
    state.update(input);
    return state.finalize();
}

} // namespace nxt::crypto
