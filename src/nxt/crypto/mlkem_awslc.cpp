#include <nxt/crypto.hpp>

#include <openssl/evp.h>
#include <openssl/nid.h>

#include <memory>

namespace nxt::crypto {
namespace {

const auto * u8_data(std::span<const std::byte> bytes) noexcept
{
    return reinterpret_cast<const uint8_t *>(bytes.data());
}

template<typename T>
auto * u8_data(T & bytes) noexcept
{
    return reinterpret_cast<uint8_t *>(bytes.data());
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

} // namespace

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
    if (!EVP_PKEY_get_raw_public_key(
            key.get(), u8_data(out.public_key), &public_len)
        || public_len != out.public_key.size())
        throw crypto_error{"failed to extract ML-KEM-768 public key"};
    if (!EVP_PKEY_get_raw_private_key(
            key.get(), u8_data(out.secret_key), &secret_len)
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
        throw crypto_error{
            "ML-KEM-768 decapsulation returned an unexpected length"};
    return out;
}

} // namespace nxt::crypto
