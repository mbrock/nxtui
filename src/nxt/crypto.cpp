#include <nxt/crypto.hpp>

#include <openssl/aead.h>
#include <openssl/curve25519.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <openssl/nid.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <memory>

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

struct aead_ctx_deleter
{
    void operator()(EVP_AEAD_CTX * ctx) const noexcept
    {
        EVP_AEAD_CTX_free(ctx);
    }
};

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

}

void random(std::span<std::byte> out)
{
    if (!out.empty() && RAND_bytes(u8_data(out), out.size()) != 1)
        throw crypto_error{"random byte generation failed"};
}

bytes random(std::size_t len)
{
    auto out = bytes(len);
    random(out);
    return out;
}

std::array<std::byte, sha256_len> sha256(std::span<const std::byte> input)
{
    auto out = std::array<std::byte, sha256_len>{};
    SHA256(u8_data(input), input.size(), u8_data(out));
    return out;
}

std::array<std::byte, sha256_len> hmac_sha256(
    std::span<const std::byte> key,
    std::span<const std::byte> data)
{
    auto out = std::array<std::byte, sha256_len>{};
    unsigned out_len = 0;
    if (!HMAC(
            EVP_sha256(),
            u8_data(key),
            static_cast<int>(key.size()),
            u8_data(data),
            data.size(),
            u8_data(out),
            &out_len)
        || out_len != out.size())
        throw crypto_error{"HMAC-SHA256 failed"};
    return out;
}

std::array<std::byte, sha256_len> hkdf_extract_sha256(
    std::span<const std::byte> salt,
    std::span<const std::byte> ikm)
{
    auto out = std::array<std::byte, sha256_len>{};
    auto out_len = out.size();
    if (!HKDF_extract(
            u8_data(out),
            &out_len,
            EVP_sha256(),
            u8_data(ikm),
            ikm.size(),
            u8_data(salt),
            salt.size())
        || out_len != out.size())
        throw crypto_error{"HKDF-SHA256 extract failed"};
    return out;
}

bytes hkdf_expand_sha256(
    std::span<const std::byte> prk,
    std::span<const std::byte> info,
    std::size_t len)
{
    auto out = bytes(len);
    if (!out.empty()
        && !HKDF_expand(
            u8_data(out),
            out.size(),
            EVP_sha256(),
            u8_data(prk),
            prk.size(),
            u8_data(info),
            info.size()))
        throw crypto_error{"HKDF-SHA256 expand failed"};
    return out;
}

std::optional<bytes> aes128gcm_open(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext)
{
    require_size(key, aes128_key_len, "AES-128-GCM key must be 16 bytes");
    require_size(nonce, aes_gcm_nonce_len, "AES-GCM nonce must be 12 bytes");
    if (ciphertext.size() < aes_gcm_tag_len)
        return std::nullopt;

    auto ctx = std::unique_ptr<EVP_AEAD_CTX, aead_ctx_deleter>{
        EVP_AEAD_CTX_new(
            EVP_aead_aes_128_gcm(),
            u8_data(key),
            key.size(),
            EVP_AEAD_DEFAULT_TAG_LENGTH)};
    if (!ctx)
        throw crypto_error{"failed to create AES-128-GCM context"};

    auto out = bytes(ciphertext.size() - aes_gcm_tag_len);
    auto out_len = std::size_t{};
    if (!EVP_AEAD_CTX_open(
            ctx.get(),
            u8_data(out),
            &out_len,
            out.size(),
            u8_data(nonce),
            nonce.size(),
            u8_data(ciphertext),
            ciphertext.size(),
            u8_data(aad),
            aad.size()))
        return std::nullopt;

    out.resize(out_len);
    return out;
}

bytes aes128gcm_seal(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> plaintext)
{
    require_size(key, aes128_key_len, "AES-128-GCM key must be 16 bytes");
    require_size(nonce, aes_gcm_nonce_len, "AES-GCM nonce must be 12 bytes");

    auto ctx = std::unique_ptr<EVP_AEAD_CTX, aead_ctx_deleter>{
        EVP_AEAD_CTX_new(
            EVP_aead_aes_128_gcm(),
            u8_data(key),
            key.size(),
            EVP_AEAD_DEFAULT_TAG_LENGTH)};
    if (!ctx)
        throw crypto_error{"failed to create AES-128-GCM context"};

    auto out = bytes(plaintext.size() + aes_gcm_tag_len);
    auto out_len = std::size_t{};
    if (!EVP_AEAD_CTX_seal(
            ctx.get(),
            u8_data(out),
            &out_len,
            out.size(),
            u8_data(nonce),
            nonce.size(),
            u8_data(plaintext),
            plaintext.size(),
            u8_data(aad),
            aad.size()))
        throw crypto_error{"AES-128-GCM seal failed"};
    out.resize(out_len);
    return out;
}

x25519_key_pair x25519_keygen()
{
    auto out = x25519_key_pair{};
    X25519_keypair(u8_data(out.public_key), u8_data(out.secret_key));
    return out;
}

std::optional<std::array<std::byte, x25519_key_len>> x25519_dh(
    std::span<const std::byte> secret_key,
    std::span<const std::byte> peer_public_key)
{
    require_size(secret_key, x25519_key_len, "X25519 secret key must be 32 bytes");
    require_size(
        peer_public_key,
        x25519_key_len,
        "X25519 public key must be 32 bytes");

    auto out = std::array<std::byte, x25519_key_len>{};
    if (!X25519(u8_data(out), u8_data(secret_key), u8_data(peer_public_key)))
        return std::nullopt;
    return out;
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

}

