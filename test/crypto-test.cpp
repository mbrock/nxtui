#include <nxt/crypto.hpp>
#include <nxtrt/crypto.hpp>
#include <nxtrt/deck.hpp>

#include "test.hpp"

#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdsa.h>
#include <openssl/nid.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::test {

using namespace boost::ut;
using namespace std::literals;

namespace {

auto bytes_from(std::string_view text)
{
    auto out = nxt::crypto::bytes(text.size());
    std::ranges::transform(text, out.begin(), [](char ch) {
        return static_cast<std::byte>(static_cast<unsigned char>(ch));
    });
    return out;
}

int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F')
        return 10 + ch - 'A';
    throw std::runtime_error{"bad hex digit"};
}

auto hex(std::string_view input)
{
    auto out = nxt::crypto::bytes{};
    out.reserve(input.size() / 2);
    auto high = -1;
    for (auto ch : input) {
        if (ch == ' ' || ch == '\n')
            continue;
        if (high < 0) {
            high = hex_digit(ch);
            continue;
        }
        out.push_back(static_cast<std::byte>((high << 4) | hex_digit(ch)));
        high = -1;
    }
    if (high >= 0)
        throw std::runtime_error{"odd hex input length"};
    return out;
}

template<std::size_t N>
bool equal_bytes(const std::array<std::byte, N> & a, std::span<const std::byte> b)
{
    return std::ranges::equal(a, b);
}

struct ec_key_deleter
{
    void operator()(EC_KEY * key) const noexcept
    {
        EC_KEY_free(key);
    }
};

nxtrt::task<void> write_sha256_sink_chunks(nxtrt::sha256_sink & sink)
{
    co_await nxtrt::write(sink, "a");
    co_await nxtrt::write(sink, "bc");
    co_await sink.flush();
}

nxtrt::task<void> write_hmac_sha256_sink_chunks(nxtrt::hmac_sha256_sink & sink)
{
    co_await nxtrt::write(sink, "Hi ");
    co_await nxtrt::write(sink, "There");
    co_await sink.flush();
}

}

static suite crypto_tests{
    "Crypto", [] {
        "fills caller-provided and owned buffers with random bytes"_test = [] {
            auto out = nxt::crypto::bytes(32);
            nxt::crypto::random(out);
            expect(std::ranges::any_of(out, [](std::byte b) {
                return b != std::byte{0};
            }));
            expect(nxt::crypto::random(7).size() == 7_ul);
        };

        "hashes bytes with SHA-256"_test = [] {
            auto digest = nxt::crypto::sha256(bytes_from("abc"));
            expect(equal_bytes(
                digest,
                hex("ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad")));
        };

        "hashes incrementally with SHA-256 state"_test = [] {
            auto state = nxt::crypto::sha256_state{};
            state.update(bytes_from("a"));
            state.update(bytes_from("b"));
            state.update(bytes_from("c"));
            expect(equal_bytes(
                state.finalize(),
                hex("ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad")));

            state.update(bytes_from("def"));
            expect(equal_bytes(
                state.finalize(),
                hex("bef57ec7f53a6d40beb640a780a639c8"
                    "3bc29ac8a9816f1fc6c5c6dcd93c4721")));
        };

        "hashes bytes through a SHA-256 sink"_test = [] {
            auto deck = nxtrt::deck{};
            auto sink = nxtrt::sha256_sink{std::size_t{2}};

            deck.sync_wait(write_sha256_sink_chunks(sink));

            expect(equal_bytes(
                sink.finalize(),
                hex("ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad")));
        };

        "authenticates bytes with HMAC-SHA256"_test = [] {
            auto key = nxt::crypto::bytes(20, std::byte{0x0b});
            auto digest = nxt::crypto::hmac_sha256(key, bytes_from("Hi There"));
            expect(equal_bytes(
                digest,
                hex("b0344c61d8db38535ca8afceaf0bf12b"
                    "881dc200c9833da726e9376c2e32cff7")));
        };

        "authenticates bytes through HMAC-SHA256 state and sink"_test = [] {
            auto key = nxt::crypto::bytes(20, std::byte{0x0b});
            auto state = nxt::crypto::hmac_sha256_state{key};
            state.update(bytes_from("Hi "));
            state.update(bytes_from("There"));
            expect(equal_bytes(
                state.finalize(),
                hex("b0344c61d8db38535ca8afceaf0bf12b"
                    "881dc200c9833da726e9376c2e32cff7")));

            auto deck = nxtrt::deck{};
            auto sink = nxtrt::hmac_sha256_sink{key, std::size_t{3}};
            deck.sync_wait(write_hmac_sha256_sink_chunks(sink));
            expect(equal_bytes(
                sink.finalize(),
                hex("b0344c61d8db38535ca8afceaf0bf12b"
                    "881dc200c9833da726e9376c2e32cff7")));
        };

        "derives RFC 5869 extract and expand outputs with SHA-256"_test = [] {
            auto ikm = nxt::crypto::bytes(22, std::byte{0x0b});
            auto salt = hex("000102030405060708090a0b0c");
            auto info = hex("f0f1f2f3f4f5f6f7f8f9");

            auto prk = nxt::crypto::hkdf_extract_sha256(salt, ikm);
            expect(equal_bytes(
                prk,
                hex("077709362c2e32df0ddc3f0dc47bba63"
                    "90b6c73bb50f9c3122ec844ad7c2b3e5")));

            auto okm = nxt::crypto::hkdf_expand_sha256(prk, info, 42);
            expect(
                okm
                == hex("3cb25f25faacd57a90434f64d0362f2a"
                       "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                       "34007208d5b887185865"));
        };

        "seals and opens AES-128-GCM ciphertexts with tags"_test = [] {
            auto key = nxt::crypto::bytes(nxt::crypto::aes128_key_len);
            auto nonce = nxt::crypto::bytes(nxt::crypto::aes_gcm_nonce_len);
            auto aad = nxt::crypto::bytes{};
            auto plaintext = nxt::crypto::bytes{};

            auto ciphertext =
                nxt::crypto::aes128gcm_seal(key, nonce, aad, plaintext);
            expect(
                ciphertext
                == hex("58e2fccefa7e3061367f1d57a4e7455a"));

            auto opened =
                nxt::crypto::aes128gcm_open(key, nonce, aad, ciphertext);
            expect(opened.has_value());
            expect(*opened == plaintext);

            ciphertext[0] ^= std::byte{1};
            expect(!nxt::crypto::aes128gcm_open(key, nonce, aad, ciphertext));
        };

        "agrees on the same X25519 shared secret"_test = [] {
            auto alice = nxt::crypto::x25519_keygen();
            auto bob = nxt::crypto::x25519_keygen();

            auto a_secret =
                nxt::crypto::x25519dh(alice.secret_key, bob.public_key);
            auto b_secret =
                nxt::crypto::x25519_dh(bob.secret_key, alice.public_key);

            expect(a_secret.has_value());
            expect(b_secret.has_value());
            expect(*a_secret == *b_secret);
        };

        "matches RFC 7748 X25519 test vectors"_test = [] {
            auto scalar = nxt::crypto::bytes(nxt::crypto::x25519_key_len);
            scalar[0] = std::byte{9};
            auto basepoint = nxt::crypto::bytes(nxt::crypto::x25519_key_len);
            basepoint[0] = std::byte{9};

            auto base_secret = nxt::crypto::x25519dh(scalar, basepoint);
            expect(base_secret.has_value());
            expect(equal_bytes(
                *base_secret,
                hex("422c8e7a6227d7bca1350b3e2bb7279f"
                    "7897b87bb6854b783c60e80311ae3079")));

            auto secret = nxt::crypto::x25519dh(
                hex("77076d0a7318a57d3c16c17251b26645"
                    "df4c2f87ebc0992ab177fba51db92c2a"),
                basepoint);

            expect(secret.has_value());
            expect(equal_bytes(
                *secret,
                hex("8520f0098930a754748b7ddcb43ef75a"
                    "0dbf3a0d26381af4eba4a98eaa9b4e6a")));
        };

        "rejects all-zero X25519 shared secrets"_test = [] {
            auto secret_key = nxt::crypto::bytes(nxt::crypto::x25519_key_len);
            secret_key[0] = std::byte{9};
            auto zero_point = nxt::crypto::bytes(nxt::crypto::x25519_key_len);

            expect(!nxt::crypto::x25519dh(secret_key, zero_point));
        };

        "round-trips ML-KEM-768 encapsulated shared secrets"_test = [] {
            auto alice = nxt::crypto::mlkem768_keygen();
            auto bob = nxt::crypto::mlkem768_encaps(alice.public_key);
            auto alice_secret =
                nxt::crypto::mlkem768_decaps(alice.secret_key, bob.ciphertext);

            expect(alice_secret.has_value());
            expect(*alice_secret == bob.shared_secret);
        };

        "verifies P-256 ECDSA signatures over SHA-256 digests"_test = [] {
            auto key = std::unique_ptr<EC_KEY, ec_key_deleter>{
                EC_KEY_new_by_curve_name(NID_X9_62_prime256v1)};
            expect(key != nullptr);
            expect(EC_KEY_generate_key(key.get()) == 1_i);

            auto message = bytes_from("nxtui crypto api");
            auto digest = nxt::crypto::sha256(message);

            auto sig = nxt::crypto::bytes(ECDSA_size(key.get()));
            auto sig_len = unsigned{};
            expect(
                ECDSA_sign(
                    0,
                    reinterpret_cast<const uint8_t *>(digest.data()),
                    digest.size(),
                    reinterpret_cast<uint8_t *>(sig.data()),
                    &sig_len,
                    key.get())
                == 1_i);
            sig.resize(sig_len);

            auto * group = EC_KEY_get0_group(key.get());
            auto * point = EC_KEY_get0_public_key(key.get());
            auto public_len = EC_POINT_point2oct(
                group,
                point,
                POINT_CONVERSION_UNCOMPRESSED,
                nullptr,
                0,
                nullptr);
            auto public_key = nxt::crypto::bytes(public_len);
            expect(
                EC_POINT_point2oct(
                    group,
                    point,
                    POINT_CONVERSION_UNCOMPRESSED,
                    reinterpret_cast<uint8_t *>(public_key.data()),
                    public_key.size(),
                    nullptr)
                == public_key.size());

            expect(nxt::crypto::ecdsa_p256_sha256_verify(
                public_key,
                message,
                sig));

            message[0] ^= std::byte{1};
            expect(!nxt::crypto::ecdsa_p256_sha256_verify(
                public_key,
                message,
                sig));
        };
    }};

}
