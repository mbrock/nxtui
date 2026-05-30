#include <nxt/crypto.hpp>

// X25519 for nxtls: the field GF(2^255 - 19), a Montgomery ladder, and scalar
// clamping. No assembly, no dispatch, no library calls in the scalar multiply.
//
// Field representation: an element is five 64-bit limbs, each holding ~51 bits
// (radix 2^51). 5 * 51 = 255, which is exactly the field width. This layout is
// the classic one: limbs have 13 spare bits of headroom, so several additions
// and a multiply can accumulate before anything overflows, and we only carry /
// reduce when we must. p = 2^255 - 19.

#include <array>
#include <algorithm>
#include <cstdint>

namespace nxt::crypto {
namespace {

using u64 = std::uint64_t;
using u128 = unsigned __int128;

struct fe { u64 v[5]; };  // field element, radix 2^51

static constexpr u64 MASK51 = (u64(1) << 51) - 1;

const auto * u8_data(std::span<const std::byte> bytes) noexcept
{
    return reinterpret_cast<const std::uint8_t *>(bytes.data());
}

template<typename T>
auto * u8_data(T & bytes) noexcept
{
    return reinterpret_cast<std::uint8_t *>(bytes.data());
}

template<typename T>
const auto * u8_data(const T & bytes) noexcept
{
    return reinterpret_cast<const std::uint8_t *>(bytes.data());
}

void require_x25519_size(std::span<const std::byte> value, const char * name)
{
    if (value.size() != x25519_key_len)
        throw crypto_error{name};
}

u64 load64_le(const std::uint8_t * p)
{
    auto out = u64{0};
    for (auto i = 0; i < 8; i++)
        out |= u64{p[i]} << (8 * i);
    return out;
}

void store64_le(std::uint8_t * out, u64 value)
{
    for (auto i = 0; i < 8; i++)
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// --- load / store between 32 little-endian bytes and 5x51 limbs ---

static fe fe_from_bytes(const std::uint8_t in[32]) {
    fe h;
    h.v[0] =  load64_le(in + 0)                     & MASK51;
    h.v[1] = (load64_le(in + 6) >> 3)               & MASK51;
    h.v[2] = (load64_le(in + 12) >> 6)              & MASK51;
    h.v[3] = (load64_le(in + 19) >> 1)              & MASK51;
    // top bit (bit 255) is ignored per RFC 7748
    h.v[4] = (load64_le(in + 24) >> 12)             & MASK51;
    return h;
}

// Fully reduce mod p, then serialize to 32 little-endian bytes.
static void fe_to_bytes(std::uint8_t out[32], const fe& f) {
    // First propagate carries so each limb is < 2^51.
    fe t = f;
    u64 c;
    c = t.v[0] >> 51; t.v[0] &= MASK51; t.v[1] += c;
    c = t.v[1] >> 51; t.v[1] &= MASK51; t.v[2] += c;
    c = t.v[2] >> 51; t.v[2] &= MASK51; t.v[3] += c;
    c = t.v[3] >> 51; t.v[3] &= MASK51; t.v[4] += c;
    c = t.v[4] >> 51; t.v[4] &= MASK51; t.v[0] += 19 * c;

    // Now t < 2p. Compute g = t - p as t + 19 - 2^255. If that final
    // subtraction does not underflow, select g; otherwise keep t.
    fe g;
    g.v[0] = t.v[0] + 19;
    c = g.v[0] >> 51; g.v[0] &= MASK51;
    g.v[1] = t.v[1] + c;
    c = g.v[1] >> 51; g.v[1] &= MASK51;
    g.v[2] = t.v[2] + c;
    c = g.v[2] >> 51; g.v[2] &= MASK51;
    g.v[3] = t.v[3] + c;
    c = g.v[3] >> 51; g.v[3] &= MASK51;
    g.v[4] = t.v[4] + c - (u64(1) << 51);

    u64 mask = (g.v[4] >> 63) - 1; // all ones iff t >= p
    g.v[4] &= MASK51;
    for (int i = 0; i < 5; i++)
        t.v[i] = (t.v[i] & ~mask) | (g.v[i] & mask);

    const u64 words[4] = {
        t.v[0]        | (t.v[1] << 51),
        (t.v[1] >> 13) | (t.v[2] << 38),
        (t.v[2] >> 26) | (t.v[3] << 25),
        (t.v[3] >> 39) | (t.v[4] << 12),
    };
    for (auto i = 0; i < 4; i++)
        store64_le(out + 8 * i, words[i]);
}

// --- field ops ---

static fe fe_add(const fe& a, const fe& b) {
    fe r;
    for (int i = 0; i < 5; i++) r.v[i] = a.v[i] + b.v[i];
    return r;
}

// a - b, kept non-negative by adding 2p (so limbs stay positive before carry).
static fe fe_sub(const fe& a, const fe& b) {
    // 2p in this radix: limb0 = 2*(2^51-19)=2^52-38, others 2*(2^51-1)=2^52-2.
    fe r;
    r.v[0] = a.v[0] + 0xFFFFFFFFFFFDAull - b.v[0];   // 2^52 - 38
    r.v[1] = a.v[1] + 0xFFFFFFFFFFFFEull - b.v[1];   // 2^52 - 2
    r.v[2] = a.v[2] + 0xFFFFFFFFFFFFEull - b.v[2];
    r.v[3] = a.v[3] + 0xFFFFFFFFFFFFEull - b.v[3];
    r.v[4] = a.v[4] + 0xFFFFFFFFFFFFEull - b.v[4];
    return r;
}

static fe fe_mul(const fe& a, const fe& b) {
    // Schoolbook 5x5, folding the 2^255 wraparound by multiplying the
    // "above the field" partial products by 19 (since 2^255 = 19 mod p).
    u128 r0 = (u128)a.v[0]*b.v[0]
            + (u128)19*a.v[1]*b.v[4]
            + (u128)19*a.v[2]*b.v[3]
            + (u128)19*a.v[3]*b.v[2]
            + (u128)19*a.v[4]*b.v[1];
    u128 r1 = (u128)a.v[0]*b.v[1]
            + (u128)a.v[1]*b.v[0]
            + (u128)19*a.v[2]*b.v[4]
            + (u128)19*a.v[3]*b.v[3]
            + (u128)19*a.v[4]*b.v[2];
    u128 r2 = (u128)a.v[0]*b.v[2]
            + (u128)a.v[1]*b.v[1]
            + (u128)a.v[2]*b.v[0]
            + (u128)19*a.v[3]*b.v[4]
            + (u128)19*a.v[4]*b.v[3];
    u128 r3 = (u128)a.v[0]*b.v[3]
            + (u128)a.v[1]*b.v[2]
            + (u128)a.v[2]*b.v[1]
            + (u128)a.v[3]*b.v[0]
            + (u128)19*a.v[4]*b.v[4];
    u128 r4 = (u128)a.v[0]*b.v[4]
            + (u128)a.v[1]*b.v[3]
            + (u128)a.v[2]*b.v[2]
            + (u128)a.v[3]*b.v[1]
            + (u128)a.v[4]*b.v[0];

    // Carry chain to bring everything back to ~51-bit limbs.
    u64 c;
    fe r;
    c = (u64)(r0 >> 51); r.v[0] = (u64)r0 & MASK51; r1 += c;
    c = (u64)(r1 >> 51); r.v[1] = (u64)r1 & MASK51; r2 += c;
    c = (u64)(r2 >> 51); r.v[2] = (u64)r2 & MASK51; r3 += c;
    c = (u64)(r3 >> 51); r.v[3] = (u64)r3 & MASK51; r4 += c;
    c = (u64)(r4 >> 51); r.v[4] = (u64)r4 & MASK51; r.v[0] += 19 * c;
    c = r.v[0] >> 51; r.v[0] &= MASK51; r.v[1] += c;
    return r;
}

static fe fe_sq(const fe& a) { return fe_mul(a, a); }

// Multiply by a small constant (used for the ladder's (A-2)/4 = 121665 step).
static fe fe_mul121665(const fe& a) {
    u128 r0 = (u128)a.v[0]*121665;
    u128 r1 = (u128)a.v[1]*121665;
    u128 r2 = (u128)a.v[2]*121665;
    u128 r3 = (u128)a.v[3]*121665;
    u128 r4 = (u128)a.v[4]*121665;
    u64 c; fe r;
    c = (u64)(r0 >> 51); r.v[0] = (u64)r0 & MASK51; r1 += c;
    c = (u64)(r1 >> 51); r.v[1] = (u64)r1 & MASK51; r2 += c;
    c = (u64)(r2 >> 51); r.v[2] = (u64)r2 & MASK51; r3 += c;
    c = (u64)(r3 >> 51); r.v[3] = (u64)r3 & MASK51; r4 += c;
    c = (u64)(r4 >> 51); r.v[4] = (u64)r4 & MASK51; r.v[0] += 19 * c;
    c = r.v[0] >> 51; r.v[0] &= MASK51; r.v[1] += c;
    return r;
}

// Constant-time conditional swap of two field elements.
static void fe_cswap(fe& a, fe& b, u64 swap) {
    u64 mask = (u64)0 - swap;  // 0 or all-ones
    for (int i = 0; i < 5; i++) {
        u64 t = mask & (a.v[i] ^ b.v[i]);
        a.v[i] ^= t;
        b.v[i] ^= t;
    }
}

// Inverse via Fermat: a^(p-2) mod p, with p-2 = 2^255 - 21.
// Standard addition chain (the one from the curve25519 reference).
static fe fe_invert(const fe& z) {
    fe z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
    z2 = fe_sq(z);
    t  = fe_sq(z2); t = fe_sq(t);
    z9 = fe_mul(t, z);
    z11 = fe_mul(z9, z2);
    t = fe_sq(z11);
    z2_5_0 = fe_mul(t, z9);

    t = fe_sq(z2_5_0); for (int i = 1; i < 5; i++) t = fe_sq(t);
    z2_10_0 = fe_mul(t, z2_5_0);

    t = fe_sq(z2_10_0); for (int i = 1; i < 10; i++) t = fe_sq(t);
    z2_20_0 = fe_mul(t, z2_10_0);

    t = fe_sq(z2_20_0); for (int i = 1; i < 20; i++) t = fe_sq(t);
    t = fe_mul(t, z2_20_0);

    t = fe_sq(t); for (int i = 1; i < 10; i++) t = fe_sq(t);
    z2_50_0 = fe_mul(t, z2_10_0);

    t = fe_sq(z2_50_0); for (int i = 1; i < 50; i++) t = fe_sq(t);
    z2_100_0 = fe_mul(t, z2_50_0);

    t = fe_sq(z2_100_0); for (int i = 1; i < 100; i++) t = fe_sq(t);
    t = fe_mul(t, z2_100_0);

    t = fe_sq(t); for (int i = 1; i < 50; i++) t = fe_sq(t);
    t = fe_mul(t, z2_50_0);

    t = fe_sq(t); t = fe_sq(t); t = fe_sq(t); t = fe_sq(t); t = fe_sq(t);
    return fe_mul(t, z11);
}

// The X25519 scalar multiplication: out = scalar * point, RFC 7748.
void x25519_raw(std::uint8_t out[32],
                const std::uint8_t scalar[32],
                const std::uint8_t point[32]) {
    std::uint8_t e[32];
    std::ranges::copy_n(scalar, 32, e);
    e[0]  &= 248;   // clamp: clear low 3 bits
    e[31] &= 127;   // clear high bit
    e[31] |= 64;    // set second-highest bit

    fe x1 = fe_from_bytes(point);
    fe x2 = {{1, 0, 0, 0, 0}};       // (x2, z2) = (1, 0)
    fe z2 = {{0, 0, 0, 0, 0}};
    fe x3 = x1;                       // (x3, z3) = (x1, 1)
    fe z3 = {{1, 0, 0, 0, 0}};

    u64 swap = 0;
    for (int t = 254; t >= 0; t--) {
        u64 bit = (e[t >> 3] >> (t & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        // The classic Montgomery ladder step (RFC 7748, section 5).
        fe A  = fe_add(x2, z2);
        fe AA = fe_sq(A);
        fe B  = fe_sub(x2, z2);
        fe BB = fe_sq(B);
        fe E  = fe_sub(AA, BB);
        fe C  = fe_add(x3, z3);
        fe D  = fe_sub(x3, z3);
        fe DA = fe_mul(D, A);
        fe CB = fe_mul(C, B);
        fe t0 = fe_add(DA, CB);
        x3 = fe_sq(t0);
        fe t1 = fe_sub(DA, CB);
        z3 = fe_mul(fe_sq(t1), x1);
        x2 = fe_mul(AA, BB);
        fe t2 = fe_mul121665(E);
        fe t3 = fe_add(AA, t2);
        z2 = fe_mul(E, t3);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe zinv = fe_invert(z2);
    fe res  = fe_mul(x2, zinv);
    fe_to_bytes(out, res);
}

bool is_all_zero(std::span<const std::byte> value)
{
    auto accum = std::byte{0};
    for (auto byte : value)
        accum |= byte;
    return accum == std::byte{0};
}

}

x25519_key_pair x25519_keygen()
{
    auto out = x25519_key_pair{};
    random(out.secret_key);

    auto basepoint = std::array<std::byte, x25519_key_len>{};
    basepoint[0] = std::byte{9};
    x25519_raw(u8_data(out.public_key), u8_data(out.secret_key), u8_data(basepoint));
    return out;
}

std::optional<std::array<std::byte, x25519_key_len>> x25519dh(
    std::span<const std::byte> secret_key,
    std::span<const std::byte> peer_public_key)
{
    require_x25519_size(secret_key, "X25519 secret key must be 32 bytes");
    require_x25519_size(peer_public_key, "X25519 public key must be 32 bytes");

    auto out = std::array<std::byte, x25519_key_len>{};
    x25519_raw(u8_data(out), u8_data(secret_key), u8_data(peer_public_key));
    if (is_all_zero(out))
        return std::nullopt;
    return out;
}

std::optional<std::array<std::byte, x25519_key_len>> x25519_dh(
    std::span<const std::byte> secret_key,
    std::span<const std::byte> peer_public_key)
{
    return x25519dh(secret_key, peer_public_key);
}

} // namespace nxt::crypto
