#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace nxt::crypto {

using bytes = std::vector<std::byte>;

inline constexpr auto sha256_len = std::size_t{32};
inline constexpr auto sha1_len = std::size_t{20};
inline constexpr auto sha1_block_len = std::size_t{64};
inline constexpr auto sha256_block_len = std::size_t{64};
inline constexpr auto aes_block_len = std::size_t{16};
inline constexpr auto aes128_key_len = std::size_t{16};
inline constexpr auto aes256_key_len = std::size_t{32};
inline constexpr auto aes256_round_key_len = std::size_t{240};
inline constexpr auto aes_ige_iv_len = std::size_t{32};
inline constexpr auto aes_gcm_nonce_len = std::size_t{12};
inline constexpr auto aes_gcm_tag_len = std::size_t{16};
inline constexpr auto x25519_key_len = std::size_t{32};
inline constexpr auto mlkem768_public_key_len = std::size_t{1184};
inline constexpr auto mlkem768_secret_key_len = std::size_t{2400};
inline constexpr auto mlkem768_ciphertext_len = std::size_t{1088};
inline constexpr auto mlkem768_shared_secret_len = std::size_t{32};

class crypto_error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct x25519_key_pair
{
    std::array<std::byte, x25519_key_len> public_key{};
    std::array<std::byte, x25519_key_len> secret_key{};
};

struct mlkem768_key_pair
{
    std::array<std::byte, mlkem768_public_key_len> public_key{};
    std::array<std::byte, mlkem768_secret_key_len> secret_key{};
};

struct mlkem768_encapsulation
{
    std::array<std::byte, mlkem768_ciphertext_len> ciphertext{};
    std::array<std::byte, mlkem768_shared_secret_len> shared_secret{};
};

class sha256_state
{
public:
    sha256_state();

    void update(std::span<const std::byte> input);
    [[nodiscard]] std::array<std::byte, sha256_len> finalize() const;

private:
    std::array<std::uint32_t, 8> state_{};
    std::array<std::byte, 64> buffer_{};
    std::uint64_t size_ = 0;
    std::size_t buffered_ = 0;
};

class sha1_state
{
public:
    sha1_state();

    void update(std::span<const std::byte> input);
    [[nodiscard]] std::array<std::byte, sha1_len> finalize() const;

private:
    std::array<std::uint32_t, 5> state_{};
    std::array<std::byte, sha1_block_len> buffer_{};
    std::uint64_t size_ = 0;
    std::size_t buffered_ = 0;
};

class hmac_sha256_state
{
public:
    explicit hmac_sha256_state(std::span<const std::byte> key);

    void update(std::span<const std::byte> input);
    [[nodiscard]] std::array<std::byte, sha256_len> finalize() const;

private:
    sha256_state inner_;
    std::array<std::byte, sha256_block_len> outer_pad_{};
};

class aes128gcm_context
{
public:
    aes128gcm_context() = default;
    explicit aes128gcm_context(std::span<const std::byte> key);

    aes128gcm_context(const aes128gcm_context &) = delete;
    aes128gcm_context & operator=(const aes128gcm_context &) = delete;

    aes128gcm_context(aes128gcm_context &&) noexcept = default;
    aes128gcm_context & operator=(aes128gcm_context &&) noexcept = default;

private:
    friend std::optional<bytes> aes128gcm_open(
        const aes128gcm_context & ctx,
        std::span<const std::byte> nonce,
        std::span<const std::byte> aad,
        std::span<const std::byte> ciphertext);
    friend std::optional<std::span<std::byte>> aes128gcm_open_in_place(
        const aes128gcm_context & ctx,
        std::span<const std::byte> nonce,
        std::span<const std::byte> aad,
        std::span<std::byte> ciphertext);
    friend bytes aes128gcm_seal(
        const aes128gcm_context & ctx,
        std::span<const std::byte> nonce,
        std::span<const std::byte> aad,
        std::span<const std::byte> plaintext);

public:
    std::array<std::byte, 176> round_keys_{};
    std::array<std::byte, 16> ghash_key_{};
    bool initialized_ = false;
};

class aes256_context
{
public:
    aes256_context() = default;
    explicit aes256_context(std::span<const std::byte> key);

    aes256_context(const aes256_context &) = delete;
    aes256_context & operator=(const aes256_context &) = delete;

    aes256_context(aes256_context &&) noexcept = default;
    aes256_context & operator=(aes256_context &&) noexcept = default;

private:
    friend void aes256_encrypt_block(
        const aes256_context & ctx,
        std::span<const std::byte> input,
        std::span<std::byte> output);
    friend void aes256_decrypt_block(
        const aes256_context & ctx,
        std::span<const std::byte> input,
        std::span<std::byte> output);
    friend void aes256_ige_encrypt(
        const aes256_context & ctx,
        std::span<const std::byte> iv,
        std::span<const std::byte> input,
        std::span<std::byte> output);
    friend void aes256_ige_decrypt(
        const aes256_context & ctx,
        std::span<const std::byte> iv,
        std::span<const std::byte> input,
        std::span<std::byte> output);

    std::array<std::byte, aes256_round_key_len> round_keys_{};
    bool initialized_ = false;
};

void random(std::span<std::byte> out);
[[nodiscard]] bytes random(std::size_t len);

[[nodiscard]] std::array<std::byte, sha256_len> sha256(
    std::span<const std::byte> input);
[[nodiscard]] std::array<std::byte, sha1_len> sha1(
    std::span<const std::byte> input);
[[nodiscard]] std::array<std::byte, sha256_len> hmac_sha256(
    std::span<const std::byte> key,
    std::span<const std::byte> data);

[[nodiscard]] std::array<std::byte, sha256_len> hkdf_extract_sha256(
    std::span<const std::byte> salt,
    std::span<const std::byte> ikm);
[[nodiscard]] bytes hkdf_expand_sha256(
    std::span<const std::byte> prk,
    std::span<const std::byte> info,
    std::size_t len);

[[nodiscard]] std::optional<bytes> aes128gcm_open(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext);
[[nodiscard]] std::optional<bytes> aes128gcm_open(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext);
[[nodiscard]] std::optional<std::span<std::byte>> aes128gcm_open_in_place(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<std::byte> ciphertext);
[[nodiscard]] std::optional<std::span<std::byte>> aes128gcm_open_in_place(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<std::byte> ciphertext);
[[nodiscard]] bytes aes128gcm_seal(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> plaintext);
[[nodiscard]] bytes aes128gcm_seal(
    const aes128gcm_context & ctx,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> plaintext);

void aes256_encrypt_block(
    const aes256_context & ctx,
    std::span<const std::byte> input,
    std::span<std::byte> output);
void aes256_decrypt_block(
    const aes256_context & ctx,
    std::span<const std::byte> input,
    std::span<std::byte> output);
void aes256_ige_encrypt(
    const aes256_context & ctx,
    std::span<const std::byte> iv,
    std::span<const std::byte> input,
    std::span<std::byte> output);
void aes256_ige_decrypt(
    const aes256_context & ctx,
    std::span<const std::byte> iv,
    std::span<const std::byte> input,
    std::span<std::byte> output);

[[nodiscard]] x25519_key_pair x25519_keygen();
[[nodiscard]] std::optional<std::array<std::byte, x25519_key_len>> x25519dh(
    std::span<const std::byte> secret_key,
    std::span<const std::byte> peer_public_key);
[[nodiscard]] std::optional<std::array<std::byte, x25519_key_len>> x25519_dh(
    std::span<const std::byte> secret_key,
    std::span<const std::byte> peer_public_key);

[[nodiscard]] mlkem768_key_pair mlkem768_keygen();
[[nodiscard]] mlkem768_encapsulation mlkem768_encaps(
    std::span<const std::byte> peer_public_key);
[[nodiscard]] std::optional<std::array<std::byte, mlkem768_shared_secret_len>>
mlkem768_decaps(
    std::span<const std::byte> secret_key,
    std::span<const std::byte> ciphertext);

[[nodiscard]] bool ecdsa_p256_sha256_verify(
    std::span<const std::byte> public_key,
    std::span<const std::byte> message,
    std::span<const std::byte> der_signature);
[[nodiscard]] bool rsa_pss_verify(
    std::span<const std::byte> spki_der,
    std::span<const std::byte> message,
    std::span<const std::byte> signature,
    unsigned digest_bits);

void rsa_raw_public_encrypt(
    std::span<const std::byte> modulus,
    unsigned exponent,
    std::span<const std::byte> input,
    std::span<std::byte> output);
void modular_exponentiate(
    std::span<const std::byte> base,
    std::span<const std::byte> exponent,
    std::span<const std::byte> modulus,
    std::span<std::byte> output);

}
