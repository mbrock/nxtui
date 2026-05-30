#include <nxt/crypto.hpp>

#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/nid.h>
#include <openssl/rsa.h>

#include <algorithm>
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

struct evp_md_ctx_deleter
{
    void operator()(EVP_MD_CTX * ctx) const noexcept
    {
        EVP_MD_CTX_free(ctx);
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
using evp_md_ctx_ptr = std::unique_ptr<EVP_MD_CTX, evp_md_ctx_deleter>;

const EVP_MD * digest_for_bits(unsigned digest_bits)
{
    switch (digest_bits) {
    case 256:
        return EVP_sha256();
    case 384:
        return EVP_sha384();
    case 512:
        return EVP_sha512();
    default:
        throw crypto_error{"unsupported RSA-PSS digest"};
    }
}

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
    auto * ptr = reinterpret_cast<const unsigned char *>(spki_der.data());
    auto key = evp_pkey_ptr{
        d2i_PUBKEY(nullptr, &ptr, static_cast<long>(spki_der.size()))};
    if (!key)
        throw crypto_error{"failed to parse RSA public key"};
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA)
        throw crypto_error{"RSA-PSS verification requires an RSA public key"};

    auto * pkey_ctx = static_cast<EVP_PKEY_CTX *>(nullptr);
    auto md_ctx = evp_md_ctx_ptr{EVP_MD_CTX_new()};
    if (!md_ctx)
        throw crypto_error{"failed to create RSA-PSS verification context"};

    auto * md = digest_for_bits(digest_bits);
    if (!EVP_DigestVerifyInit(
            md_ctx.get(), &pkey_ctx, md, nullptr, key.get()))
        throw crypto_error{"failed to initialize RSA-PSS verification"};
    if (!EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, RSA_PKCS1_PSS_PADDING))
        throw crypto_error{"failed to configure RSA-PSS padding"};
    if (!EVP_PKEY_CTX_set_rsa_pss_saltlen(
            pkey_ctx, RSA_PSS_SALTLEN_DIGEST))
        throw crypto_error{"failed to configure RSA-PSS salt length"};

    if (!EVP_DigestVerifyUpdate(md_ctx.get(), u8_data(message), message.size()))
        throw crypto_error{"failed to update RSA-PSS verification"};
    return EVP_DigestVerifyFinal(
        md_ctx.get(), u8_data(signature), signature.size())
           == 1;
}

}
