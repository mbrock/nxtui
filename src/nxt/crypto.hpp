#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace nxt::crypto {

using bytes = std::vector<std::byte>;

inline constexpr auto sha256_len = std::size_t{32};
inline constexpr auto aes128_key_len = std::size_t{16};
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

void random(std::span<std::byte> out);
[[nodiscard]] bytes random(std::size_t len);

[[nodiscard]] std::array<std::byte, sha256_len> sha256(
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
[[nodiscard]] bytes aes128gcm_seal(
    std::span<const std::byte> key,
    std::span<const std::byte> nonce,
    std::span<const std::byte> aad,
    std::span<const std::byte> plaintext);

[[nodiscard]] x25519_key_pair x25519_keygen();
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

}

