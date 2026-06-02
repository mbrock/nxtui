#include <nxt/crypto.hpp>

#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <cerrno>
#include <cstring>
#include <memory>

#if defined(__linux__)
#    include <sys/random.h>
#else
#    include <stdlib.h>
#endif

namespace nxt::crypto {
namespace {

auto * u8_data(std::span<std::byte> bytes) noexcept
{
    return reinterpret_cast<uint8_t *>(bytes.data());
}

class der_cursor
{
public:
    explicit der_cursor(std::span<const std::byte> input)
        : input_(input)
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return offset_ == input_.size();
    }

    [[nodiscard]] std::span<const std::byte> remaining() const noexcept
    {
        return input_.subspan(offset_);
    }

    std::byte take_u8()
    {
        if (offset_ >= input_.size())
            throw crypto_error{"truncated DER value"};
        return input_[offset_++];
    }

    std::span<const std::byte> take(std::size_t n)
    {
        if (n > input_.size() - offset_)
            throw crypto_error{"truncated DER value"};
        auto out = input_.subspan(offset_, n);
        offset_ += n;
        return out;
    }

    std::span<const std::byte> take_value(std::byte expected_tag)
    {
        auto tag = take_u8();
        if (tag != expected_tag)
            throw crypto_error{"unexpected DER tag"};
        auto len = take_length();
        return take(len);
    }

private:
    std::size_t take_length()
    {
        auto first = std::to_integer<unsigned char>(take_u8());
        if ((first & 0x80) == 0)
            return first;

        auto octets = static_cast<std::size_t>(first & 0x7f);
        if (octets == 0 || octets > sizeof(std::size_t))
            throw crypto_error{"unsupported DER length"};
        if (offset_ + octets > input_.size())
            throw crypto_error{"truncated DER length"};

        auto out = std::size_t{0};
        for (std::size_t i = 0; i < octets; i++)
            out = (out << 8) | std::to_integer<unsigned char>(take_u8());
        if (out < 128)
            throw crypto_error{"non-minimal DER length"};
        return out;
    }

    std::span<const std::byte> input_;
    std::size_t offset_ = 0;
};

bool der_equal(
    std::span<const std::byte> value,
    std::initializer_list<unsigned char> expected)
{
    if (value.size() != expected.size())
        return false;
    auto it = expected.begin();
    for (auto byte : value) {
        if (std::to_integer<unsigned char>(byte) != *it++)
            return false;
    }
    return true;
}

std::span<const std::byte> der_positive_integer(std::span<const std::byte> in)
{
    if (in.empty())
        throw crypto_error{"empty DER integer"};
    if (in.front() == std::byte{0}) {
        in = in.subspan(1);
        if (in.empty() || (std::to_integer<unsigned char>(in.front()) & 0x80)
            == 0)
            throw crypto_error{"non-minimal DER integer"};
    } else if ((std::to_integer<unsigned char>(in.front()) & 0x80) != 0) {
        throw crypto_error{"negative DER integer"};
    }
    return in;
}

struct rsa_public_key
{
    bytes modulus;
    unsigned exponent = 0;
};

rsa_public_key parse_rsa_spki(std::span<const std::byte> spki_der)
{
    auto spki = der_cursor{der_cursor{spki_der}.take_value(std::byte{0x30})};
    auto algorithm = der_cursor{spki.take_value(std::byte{0x30})};
    auto oid = algorithm.take_value(std::byte{0x06});
    if (!der_equal(oid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01}))
        throw crypto_error{"RSA-PSS verification requires an RSA public key"};
    if (!algorithm.empty()) {
        auto null = algorithm.take_value(std::byte{0x05});
        if (!null.empty() || !algorithm.empty())
            throw crypto_error{"unsupported RSA public key parameters"};
    }

    auto public_key = der_cursor{spki.take_value(std::byte{0x03})};
    if (public_key.take_u8() != std::byte{0})
        throw crypto_error{"unsupported RSA BIT STRING"};
    auto rsa = der_cursor{
        der_cursor{public_key.remaining()}.take_value(std::byte{0x30})};
    auto modulus = der_positive_integer(rsa.take_value(std::byte{0x02}));
    auto exponent_bytes = der_positive_integer(rsa.take_value(std::byte{0x02}));
    if (!rsa.empty() || !spki.empty())
        throw crypto_error{"trailing data in RSA public key"};
    if (exponent_bytes.empty() || exponent_bytes.size() > sizeof(unsigned))
        throw crypto_error{"unsupported RSA public exponent"};

    auto out = rsa_public_key{};
    out.modulus.assign(modulus.begin(), modulus.end());
    for (auto b : exponent_bytes)
        out.exponent = (out.exponent << 8) | std::to_integer<unsigned char>(b);
    if (out.exponent < 3 || (out.exponent & 1) == 0)
        throw crypto_error{"unsupported RSA public exponent"};
    return out;
}

boost::multiprecision::cpp_int int_from_bytes(std::span<const std::byte> in)
{
    auto out = boost::multiprecision::cpp_int{0};
    for (auto b : in)
        out = (out << 8) | std::to_integer<unsigned char>(b);
    return out;
}

boost::multiprecision::cpp_int hex_int(const char * text)
{
    auto out = boost::multiprecision::cpp_int{0};
    for (auto * p = text; *p != '\0'; p++) {
        auto ch = *p;
        if (ch == ' ' || ch == '\n')
            continue;
        auto digit = 0;
        if (ch >= '0' && ch <= '9')
            digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            digit = 10 + ch - 'a';
        else if (ch >= 'A' && ch <= 'F')
            digit = 10 + ch - 'A';
        else
            throw crypto_error{"bad hex integer"};
        out = (out << 4) | digit;
    }
    return out;
}

boost::multiprecision::cpp_int mod(
    boost::multiprecision::cpp_int value,
    const boost::multiprecision::cpp_int & modulus)
{
    value %= modulus;
    if (value < 0)
        value += modulus;
    return value;
}

boost::multiprecision::cpp_int mod_inverse(
    boost::multiprecision::cpp_int value,
    boost::multiprecision::cpp_int modulus)
{
    if (modulus == 0)
        throw crypto_error{"zero modular inverse modulus"};
    value = mod(value, modulus);
    if (value == 0)
        throw crypto_error{"zero has no modular inverse"};
    auto old_r = modulus;
    auto r = value;
    auto old_s = boost::multiprecision::cpp_int{0};
    auto s = boost::multiprecision::cpp_int{1};
    while (r != 0) {
        auto q = boost::multiprecision::cpp_int{old_r / r};
        auto next_r = boost::multiprecision::cpp_int{old_r - q * r};
        old_r = r;
        r = next_r;
        auto next_s = boost::multiprecision::cpp_int{old_s - q * s};
        old_s = s;
        s = next_s;
    }
    if (old_r != 1)
        throw crypto_error{"value has no modular inverse"};
    return mod(old_s, modulus);
}

bytes bytes_from_int(boost::multiprecision::cpp_int value, std::size_t len)
{
    auto out = bytes(len);
    for (std::size_t i = 0; i < len; i++) {
        out[len - 1 - i] = static_cast<std::byte>(
            static_cast<unsigned>(value & 0xff));
        value >>= 8;
    }
    if (value != 0)
        throw crypto_error{"RSA value is too large"};
    return out;
}

void write_int_bytes(
    boost::multiprecision::cpp_int value,
    std::span<std::byte> out)
{
    for (std::size_t i = 0; i < out.size(); i++) {
        out[out.size() - 1 - i] = static_cast<std::byte>(
            static_cast<unsigned>(value & 0xff));
        value >>= 8;
    }
    if (value != 0)
        throw crypto_error{"RSA value is too large"};
}

std::size_t bit_length(std::span<const std::byte> in)
{
    auto i = std::size_t{0};
    while (i < in.size() && in[i] == std::byte{0})
        i++;
    if (i == in.size())
        return 0;
    auto first = std::to_integer<unsigned char>(in[i]);
    auto bits = std::size_t{0};
    while (first != 0) {
        bits++;
        first >>= 1;
    }
    return (in.size() - i - 1) * 8 + bits;
}

boost::multiprecision::cpp_int pow_mod(
    boost::multiprecision::cpp_int base,
    unsigned exponent,
    const boost::multiprecision::cpp_int & modulus)
{
    auto out = boost::multiprecision::cpp_int{1};
    base %= modulus;
    while (exponent != 0) {
        if ((exponent & 1) != 0)
            out = (out * base) % modulus;
        exponent >>= 1;
        if (exponent != 0)
            base = (base * base) % modulus;
    }
    return out;
}

boost::multiprecision::cpp_int pow_mod(
    boost::multiprecision::cpp_int base,
    boost::multiprecision::cpp_int exponent,
    const boost::multiprecision::cpp_int & modulus)
{
    auto out = boost::multiprecision::cpp_int{1};
    base %= modulus;
    while (exponent != 0) {
        if ((exponent & 1) != 0)
            out = (out * base) % modulus;
        exponent >>= 1;
        if (exponent != 0)
            base = (base * base) % modulus;
    }
    return out;
}

bytes mgf1_sha256(std::span<const std::byte> seed, std::size_t len)
{
    auto out = bytes{};
    out.reserve(len);
    for (auto counter = std::uint32_t{0}; out.size() < len; counter++) {
        auto state = sha256_state{};
        state.update(seed);
        auto c = std::array<std::byte, 4>{
            static_cast<std::byte>((counter >> 24) & 0xff),
            static_cast<std::byte>((counter >> 16) & 0xff),
            static_cast<std::byte>((counter >> 8) & 0xff),
            static_cast<std::byte>(counter & 0xff)};
        state.update(c);
        auto digest = state.finalize();
        auto take = std::min(digest.size(), len - out.size());
        out.insert(out.end(), digest.begin(), digest.begin() + take);
    }
    return out;
}

bool constant_time_equal(
    std::span<const std::byte> a,
    std::span<const std::byte> b)
{
    if (a.size() != b.size())
        return false;
    auto diff = unsigned{0};
    for (std::size_t i = 0; i < a.size(); i++)
        diff |= std::to_integer<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

struct p256_point
{
    boost::multiprecision::cpp_int x;
    boost::multiprecision::cpp_int y;
    bool infinity = true;
};

const boost::multiprecision::cpp_int & p256_p()
{
    static const auto value = hex_int(
        "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
    return value;
}

const boost::multiprecision::cpp_int & p256_n()
{
    static const auto value = hex_int(
        "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    return value;
}

const p256_point & p256_g()
{
    static const auto value = p256_point{
        .x = hex_int(
            "6b17d1f2e12c4247f8bce6e563a440f2"
            "77037d812deb33a0f4a13945d898c296"),
        .y = hex_int(
            "4fe342e2fe1a7f9b8ee7eb4a7c0f9e16"
            "2bce33576b315ececbb6406837bf51f5"),
        .infinity = false};
    return value;
}

p256_point p256_add(const p256_point & a, const p256_point & b)
{
    if (a.infinity)
        return b;
    if (b.infinity)
        return a;

    const auto & p = p256_p();
    if (a.x == b.x) {
        if (mod(a.y + b.y, p) == 0)
            return {};
        auto denominator = mod(2 * a.y, p);
        if (denominator == 0)
            return {};
        auto lambda = mod(
            (3 * a.x * a.x - 3) * mod_inverse(denominator, p),
            p);
        auto x = mod(lambda * lambda - 2 * a.x, p);
        auto y = mod(lambda * (a.x - x) - a.y, p);
        return {.x = x, .y = y, .infinity = false};
    }

    auto denominator = mod(b.x - a.x, p);
    if (denominator == 0)
        return {};
    auto lambda = mod((b.y - a.y) * mod_inverse(denominator, p), p);
    auto x = mod(lambda * lambda - a.x - b.x, p);
    auto y = mod(lambda * (a.x - x) - a.y, p);
    return {.x = x, .y = y, .infinity = false};
}

p256_point p256_mul(boost::multiprecision::cpp_int scalar, p256_point point)
{
    auto out = p256_point{};
    while (scalar > 0) {
        if ((scalar & 1) != 0)
            out = p256_add(out, point);
        scalar >>= 1;
        if (scalar != 0)
            point = p256_add(point, point);
    }
    return out;
}

bool p256_on_curve(const p256_point & point)
{
    if (point.infinity)
        return false;
    const auto & p = p256_p();
    auto left = mod(point.y * point.y, p);
    auto right = mod(point.x * point.x * point.x - 3 * point.x
                         + hex_int(
                             "5ac635d8aa3a93e7b3ebbd55769886bc"
                             "651d06b0cc53b0f63bce3c3e27d2604b"),
                     p);
    return left == right;
}

struct ecdsa_signature
{
    boost::multiprecision::cpp_int r;
    boost::multiprecision::cpp_int s;
};

ecdsa_signature parse_ecdsa_der(std::span<const std::byte> der_signature)
{
    auto sig =
        der_cursor{der_cursor{der_signature}.take_value(std::byte{0x30})};
    auto r = int_from_bytes(
        der_positive_integer(sig.take_value(std::byte{0x02})));
    auto s = int_from_bytes(
        der_positive_integer(sig.take_value(std::byte{0x02})));
    if (!sig.empty())
        throw crypto_error{"trailing data in ECDSA signature"};
    return {.r = r, .s = s};
}

}

std::optional<bytes> aes128gcm_open_impl(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext);
std::optional<std::span<std::byte>> aes128gcm_open_in_place_impl(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<std::byte> ciphertext);
bytes aes128gcm_seal_impl(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> plaintext);

void random(std::span<std::byte> out)
{
    auto rest = out;
    while (!rest.empty()) {
#if defined(__linux__)
        auto n = getrandom(u8_data(rest), rest.size(), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw crypto_error{
                std::string{"random byte generation failed: "}
                + std::strerror(errno)};
        }
        if (n == 0)
            throw crypto_error{"random byte generation made no progress"};
        rest = rest.subspan(static_cast<std::size_t>(n));
#else
        arc4random_buf(rest.data(), rest.size());
        rest = {};
#endif
    }
}

bytes random(std::size_t len)
{
    auto out = bytes(len);
    random(out);
    return out;
}

std::array<std::byte, sha256_len> hkdf_extract_sha256(
    std::span<const std::byte> salt,
    std::span<const std::byte> ikm)
{
    auto zero_salt = std::array<std::byte, sha256_len>{};
    if (salt.empty())
        salt = zero_salt;
    return hmac_sha256(salt, ikm);
}

bytes hkdf_expand_sha256(
    std::span<const std::byte> prk,
    std::span<const std::byte> info,
    std::size_t len)
{
    if (len > 255 * sha256_len)
        throw crypto_error{"HKDF-SHA256 output is too long"};

    auto out = bytes(len);
    auto previous = std::array<std::byte, sha256_len>{};
    auto previous_size = std::size_t{0};
    auto written = std::size_t{0};
    auto counter = std::byte{1};
    while (written != out.size()) {
        auto hmac = hmac_sha256_state{prk};
        hmac.update(std::span{previous}.first(previous_size));
        hmac.update(info);
        hmac.update(std::span{&counter, 1});
        previous = hmac.finalize();

        auto take = std::min(out.size() - written, previous.size());
        std::ranges::copy(
            std::span{previous}.first(take),
            out.begin() + static_cast<std::ptrdiff_t>(written));
        written += take;
        counter = static_cast<std::byte>(
            std::to_integer<unsigned char>(counter) + 1);
        previous_size = previous.size();
    }
    return out;
}

std::optional<bytes> aes128gcm_open(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext)
{
    auto ctx = aes128gcm_context{key};
    return aes128gcm_open(ctx, nonce, aad, ciphertext);
}

std::optional<bytes> aes128gcm_open(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext)
{
    return aes128gcm_open_impl(ctx, nonce, aad, ciphertext);
}

std::optional<std::span<std::byte>> aes128gcm_open_in_place(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<std::byte> ciphertext)
{
    auto ctx = aes128gcm_context{key};
    return aes128gcm_open_in_place(ctx, nonce, aad, ciphertext);
}

std::optional<std::span<std::byte>> aes128gcm_open_in_place(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<std::byte> ciphertext)
{
    return aes128gcm_open_in_place_impl(ctx, nonce, aad, ciphertext);
}

bytes aes128gcm_seal(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> plaintext)
{
    auto ctx = aes128gcm_context{key};
    return aes128gcm_seal(ctx, nonce, aad, plaintext);
}

bytes aes128gcm_seal(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> plaintext)
{
    return aes128gcm_seal_impl(ctx, nonce, aad, plaintext);
}

bool ecdsa_p256_sha256_verify(
    std::span<const std::byte> public_key,
    std::span<const std::byte> message,
    std::span<const std::byte> der_signature)
{
    if (public_key.size() != 65 || public_key.front() != std::byte{0x04})
        return false;

    auto q = p256_point{
        .x = int_from_bytes(public_key.subspan(1, 32)),
        .y = int_from_bytes(public_key.subspan(33, 32)),
        .infinity = false};
    if (!p256_on_curve(q))
        return false;

    auto sig = parse_ecdsa_der(der_signature);
    const auto & n = p256_n();
    if (sig.r <= 0 || sig.r >= n || sig.s <= 0 || sig.s >= n)
        return false;

    auto digest = sha256(message);
    auto z = int_from_bytes(digest);
    auto w = mod_inverse(sig.s, n);
    auto u1 = mod(z * w, n);
    auto u2 = mod(sig.r * w, n);
    auto point = p256_add(p256_mul(u1, p256_g()), p256_mul(u2, q));
    if (point.infinity)
        return false;
    return mod(point.x, n) == sig.r;
}

bool rsa_pss_verify(
    std::span<const std::byte> spki_der,
    std::span<const std::byte> message,
    std::span<const std::byte> signature,
    unsigned digest_bits)
{
    if (digest_bits != 256)
        throw crypto_error{"unsupported RSA-PSS digest"};

    auto key = parse_rsa_spki(spki_der);
    auto mod_bits = bit_length(key.modulus);
    auto em_bits = mod_bits - 1;
    auto em_len = (em_bits + 7) / 8;
    auto h_len = sha256_len;
    auto salt_len = h_len;
    if (em_len < h_len + salt_len + 2)
        return false;
    if (signature.size() != key.modulus.size())
        return false;

    auto n = int_from_bytes(key.modulus);
    auto s = int_from_bytes(signature);
    if (s >= n)
        return false;

    auto m = pow_mod(s, key.exponent, n);
    auto full_em = bytes_from_int(m, signature.size());
    auto leading = signature.size() - em_len;
    if (!std::ranges::all_of(
            std::span{full_em}.first(leading),
            [](std::byte b) { return b == std::byte{0}; }))
        return false;
    auto em = std::span{full_em}.last(em_len);
    if (em.back() != std::byte{0xbc})
        return false;

    auto db_len = em_len - h_len - 1;
    auto masked_db = em.first(db_len);
    auto h = em.subspan(db_len, h_len);
    auto unused_bits = 8 * em_len - em_bits;
    if (unused_bits != 0) {
        auto mask = static_cast<unsigned char>(0xff << (8 - unused_bits));
        if ((std::to_integer<unsigned char>(masked_db.front()) & mask) != 0)
            return false;
    }

    auto db_mask = mgf1_sha256(h, db_len);
    auto db = bytes(db_len);
    for (std::size_t i = 0; i < db.size(); i++)
        db[i] = masked_db[i] ^ db_mask[i];
    if (unused_bits != 0) {
        auto mask = static_cast<unsigned char>(0xff >> unused_bits);
        db.front() &= static_cast<std::byte>(mask);
    }

    auto ps_len = em_len - h_len - salt_len - 2;
    if (!std::ranges::all_of(
            std::span{db}.first(ps_len),
            [](std::byte b) { return b == std::byte{0}; }))
        return false;
    if (db[ps_len] != std::byte{0x01})
        return false;
    auto salt = std::span{db}.last(salt_len);

    auto message_hash = sha256(message);
    auto state = sha256_state{};
    auto zeros = std::array<std::byte, 8>{};
    state.update(zeros);
    state.update(message_hash);
    state.update(salt);
    auto want = state.finalize();
    return constant_time_equal(want, h);
}

void rsa_raw_public_encrypt(
    std::span<const std::byte> modulus,
    unsigned exponent,
    std::span<const std::byte> input,
    std::span<std::byte> output)
{
    if (modulus.empty())
        throw crypto_error{"RSA modulus is empty"};
    if (output.size() != modulus.size())
        throw crypto_error{"RSA output must match modulus size"};
    if (input.size() > modulus.size())
        throw crypto_error{"RSA input is too large"};
    if (exponent < 3 || (exponent & 1) == 0)
        throw crypto_error{"unsupported RSA public exponent"};

    auto n = int_from_bytes(modulus);
    auto m = int_from_bytes(input);
    if (m >= n)
        throw crypto_error{"RSA input is not below modulus"};

    write_int_bytes(pow_mod(m, exponent, n), output);
}

void modular_exponentiate(
    std::span<const std::byte> base,
    std::span<const std::byte> exponent,
    std::span<const std::byte> modulus,
    std::span<std::byte> output)
{
    if (modulus.empty())
        throw crypto_error{"modular exponent modulus is empty"};
    if (output.empty())
        throw crypto_error{"modular exponent output is empty"};

    auto n = int_from_bytes(modulus);
    auto b = int_from_bytes(base);
    auto e = int_from_bytes(exponent);
    write_int_bytes(pow_mod(b, e, n), output);
}

}
