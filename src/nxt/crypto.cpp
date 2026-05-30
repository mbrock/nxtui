#include <nxt/crypto.hpp>

#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/nid.h>

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

const auto * u8_data(std::span<const std::byte> bytes) noexcept
{
    return reinterpret_cast<const uint8_t *>(bytes.data());
}

auto * u8_data(std::span<std::byte> bytes) noexcept
{
    return reinterpret_cast<uint8_t *>(bytes.data());
}

template<typename T>
auto * u8_data(T & bytes) noexcept
{
    return reinterpret_cast<uint8_t *>(bytes.data());
}

template<typename T>
const auto * u8_data(const T & bytes) noexcept
{
    return reinterpret_cast<const uint8_t *>(bytes.data());
}

void require_size(
    std::span<const std::byte> value,
    std::size_t expected,
    const char * name)
{
    if (value.size() != expected)
        throw crypto_error{name};
}

struct evp_pkey_deleter
{
    void operator()(EVP_PKEY * key) const noexcept
    {
        EVP_PKEY_free(key);
    }
};

struct evp_pkey_ctx_deleter
{
    void operator()(EVP_PKEY_CTX * ctx) const noexcept
    {
        EVP_PKEY_CTX_free(ctx);
    }
};

struct ec_key_deleter
{
    void operator()(EC_KEY * key) const noexcept
    {
        EC_KEY_free(key);
    }
};

struct ec_point_deleter
{
    void operator()(EC_POINT * point) const noexcept
    {
        EC_POINT_free(point);
    }
};

using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>;
using evp_pkey_ctx_ptr = std::unique_ptr<EVP_PKEY_CTX, evp_pkey_ctx_deleter>;

evp_pkey_ctx_ptr mlkem768_keygen_context()
{
    auto ctx = evp_pkey_ctx_ptr{EVP_PKEY_CTX_new_id(EVP_PKEY_KEM, nullptr)};
    if (!ctx)
        throw crypto_error{"failed to create ML-KEM context"};
    if (!EVP_PKEY_CTX_kem_set_params(ctx.get(), NID_MLKEM768))
        throw crypto_error{"failed to configure ML-KEM-768"};
    if (!EVP_PKEY_keygen_init(ctx.get()))
        throw crypto_error{"failed to initialize ML-KEM-768 keygen"};
    return ctx;
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

}

std::optional<bytes> aes128gcm_open_impl(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext);
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

mlkem768_key_pair mlkem768_keygen()
{
    auto ctx = mlkem768_keygen_context();
    auto raw = static_cast<EVP_PKEY *>(nullptr);
    if (!EVP_PKEY_keygen(ctx.get(), &raw) || !raw)
        throw crypto_error{"ML-KEM-768 keygen failed"};
    auto key = evp_pkey_ptr{raw};

    auto out = mlkem768_key_pair{};
    auto public_len = out.public_key.size();
    auto secret_len = out.secret_key.size();
    if (!EVP_PKEY_get_raw_public_key(key.get(), u8_data(out.public_key), &public_len)
        || public_len != out.public_key.size())
        throw crypto_error{"failed to extract ML-KEM-768 public key"};
    if (!EVP_PKEY_get_raw_private_key(key.get(), u8_data(out.secret_key), &secret_len)
        || secret_len != out.secret_key.size())
        throw crypto_error{"failed to extract ML-KEM-768 secret key"};
    return out;
}

mlkem768_encapsulation mlkem768_encaps(std::span<const std::byte> peer_public_key)
{
    require_size(
        peer_public_key,
        mlkem768_public_key_len,
        "ML-KEM-768 public key must be 1184 bytes");

    auto key = evp_pkey_ptr{EVP_PKEY_kem_new_raw_public_key(
        NID_MLKEM768,
        u8_data(peer_public_key),
        peer_public_key.size())};
    if (!key)
        throw crypto_error{"invalid ML-KEM-768 public key"};

    auto ctx = evp_pkey_ctx_ptr{EVP_PKEY_CTX_new(key.get(), nullptr)};
    if (!ctx)
        throw crypto_error{"failed to create ML-KEM-768 encapsulation context"};

    auto out = mlkem768_encapsulation{};
    auto ciphertext_len = out.ciphertext.size();
    auto shared_secret_len = out.shared_secret.size();
    if (!EVP_PKEY_encapsulate(
            ctx.get(),
            u8_data(out.ciphertext),
            &ciphertext_len,
            u8_data(out.shared_secret),
            &shared_secret_len)
        || ciphertext_len != out.ciphertext.size()
        || shared_secret_len != out.shared_secret.size())
        throw crypto_error{"ML-KEM-768 encapsulation failed"};
    return out;
}

std::optional<std::array<std::byte, mlkem768_shared_secret_len>> mlkem768_decaps(
    std::span<const std::byte> secret_key,
    std::span<const std::byte> ciphertext)
{
    require_size(
        secret_key,
        mlkem768_secret_key_len,
        "ML-KEM-768 secret key must be 2400 bytes");
    require_size(
        ciphertext,
        mlkem768_ciphertext_len,
        "ML-KEM-768 ciphertext must be 1088 bytes");

    auto key = evp_pkey_ptr{EVP_PKEY_kem_new_raw_secret_key(
        NID_MLKEM768,
        u8_data(secret_key),
        secret_key.size())};
    if (!key)
        throw crypto_error{"invalid ML-KEM-768 secret key"};

    auto ctx = evp_pkey_ctx_ptr{EVP_PKEY_CTX_new(key.get(), nullptr)};
    if (!ctx)
        throw crypto_error{"failed to create ML-KEM-768 decapsulation context"};

    auto out = std::array<std::byte, mlkem768_shared_secret_len>{};
    auto out_len = out.size();
    if (!EVP_PKEY_decapsulate(
            ctx.get(),
            u8_data(out),
            &out_len,
            u8_data(ciphertext),
            ciphertext.size()))
        return std::nullopt;
    if (out_len != out.size())
        throw crypto_error{"ML-KEM-768 decapsulation returned an unexpected length"};
    return out;
}

bool ecdsa_p256_sha256_verify(
    std::span<const std::byte> public_key,
    std::span<const std::byte> message,
    std::span<const std::byte> der_signature)
{
    auto key = std::unique_ptr<EC_KEY, ec_key_deleter>{
        EC_KEY_new_by_curve_name(NID_X9_62_prime256v1)};
    if (!key)
        throw crypto_error{"failed to create P-256 key"};

    const auto * group = EC_KEY_get0_group(key.get());
    auto point = std::unique_ptr<EC_POINT, ec_point_deleter>{
        EC_POINT_new(group)};
    if (!point)
        throw crypto_error{"failed to create P-256 point"};
    if (!EC_POINT_oct2point(
            group,
            point.get(),
            u8_data(public_key),
            public_key.size(),
            nullptr))
        return false;
    if (!EC_KEY_set_public_key(key.get(), point.get()))
        throw crypto_error{"failed to set P-256 public key"};

    auto digest = sha256(message);
    return ECDSA_verify(
        0,
        u8_data(digest),
        digest.size(),
        u8_data(der_signature),
        der_signature.size(),
        key.get())
           == 1;
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

}
