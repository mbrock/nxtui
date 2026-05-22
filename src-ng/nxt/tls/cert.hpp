#pragma once

#include <nxt/tls.hpp>

#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/evp.h>
#include <openssl/nid.h>
#include <openssl/x509.h>

#include <memory>

namespace nxt::tls {

struct x509_deleter
{
    void operator()(X509 * cert) const noexcept
    {
        X509_free(cert);
    }
};

struct evp_pkey_deleter
{
    void operator()(EVP_PKEY * key) const noexcept
    {
        EVP_PKEY_free(key);
    }
};

struct ec_key_deleter
{
    void operator()(EC_KEY * key) const noexcept
    {
        EC_KEY_free(key);
    }
};

struct tls13_certificate
{
    bytes leaf_der;
    std::vector<bytes> chain_der;
};

struct tls13_certificate_verify
{
    std::uint16_t scheme = 0;
    bytes signature;
};

inline tls13_certificate parse_tls13_certificate(std::span<const std::byte> message)
{
    auto cursor = byte_cursor{message};
    require_tls(cursor.take_u8() == 11, "expected certificate message");
    auto length = cursor.take_u24();
    auto body = byte_cursor{cursor.take(length)};
    require_tls(cursor.empty(), "unexpected bytes after certificate message");

    auto request_context_len = body.take_u8();
    body.take(request_context_len);

    auto entries = byte_cursor{body.take(body.take_u24())};
    require_tls(body.empty(), "unexpected bytes after certificate list");
    require_tls(!entries.empty(), "certificate list is empty");

    auto certificate = tls13_certificate{};
    while (!entries.empty()) {
        auto cert = entries.take(entries.take_u24());
        auto extensions_len = entries.take_u16();
        entries.take(extensions_len);
        certificate.chain_der.emplace_back(cert.begin(), cert.end());
    }
    certificate.leaf_der = certificate.chain_der.front();
    return certificate;
}

inline tls13_certificate_verify parse_tls13_certificate_verify(
    std::span<const std::byte> message)
{
    auto cursor = byte_cursor{message};
    require_tls(cursor.take_u8() == 15, "expected certificate_verify message");
    auto length = cursor.take_u24();
    auto body = byte_cursor{cursor.take(length)};
    require_tls(cursor.empty(), "unexpected bytes after certificate_verify message");

    auto scheme = body.take_u16();
    auto signature = body.take(body.take_u16());
    require_tls(body.empty(), "unexpected bytes after certificate_verify signature");
    return tls13_certificate_verify{
        .scheme = scheme,
        .signature = bytes{signature.begin(), signature.end()},
    };
}

inline bytes extract_p256_public_key_from_certificate(std::span<const std::byte> der)
{
    auto * ptr = reinterpret_cast<const unsigned char *>(der.data());
    auto cert = std::unique_ptr<X509, x509_deleter>{
        d2i_X509(nullptr, &ptr, static_cast<long>(der.size()))};
    require_tls(cert != nullptr, "failed to parse leaf certificate");

    auto key = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>{
        X509_get_pubkey(cert.get())};
    require_tls(key != nullptr, "leaf certificate has no public key");
    require_tls(
        EVP_PKEY_base_id(key.get()) == EVP_PKEY_EC,
        "leaf certificate public key is not EC");

    auto ec_key = std::unique_ptr<EC_KEY, ec_key_deleter>{
        EVP_PKEY_get1_EC_KEY(key.get())};
    require_tls(ec_key != nullptr, "failed to extract EC public key");

    auto * group = EC_KEY_get0_group(ec_key.get());
    auto * point = EC_KEY_get0_public_key(ec_key.get());
    require_tls(group != nullptr && point != nullptr, "EC public key is incomplete");
    require_tls(
        EC_GROUP_get_curve_name(group) == NID_X9_62_prime256v1,
        "leaf certificate public key is not P-256");

    auto len = EC_POINT_point2oct(
        group, point, POINT_CONVERSION_UNCOMPRESSED, nullptr, 0, nullptr);
    require_tls(len > 0, "failed to measure P-256 public key");

    auto out = bytes(len);
    auto written = EC_POINT_point2oct(
        group,
        point,
        POINT_CONVERSION_UNCOMPRESSED,
        reinterpret_cast<unsigned char *>(out.data()),
        out.size(),
        nullptr);
    require_tls(written == out.size(), "failed to encode P-256 public key");
    return out;
}

inline bytes certificate_verify_message(std::span<const std::byte> transcript)
{
    auto out = bytes(64, std::byte{0x20});
    put_text(out, "TLS 1.3, server CertificateVerify");
    put_u8(out, 0);
    auto hash = nxt::crypto::sha256(transcript);
    put_bytes(out, hash);
    return out;
}

inline bool verify_certificate_verify(
    std::span<const std::byte> p256_public_key,
    std::span<const std::byte> transcript_through_certificate,
    tls13_certificate_verify const & certificate_verify)
{
    require_tls(
        certificate_verify.scheme == 0x0403,
        "server used an unsupported CertificateVerify signature scheme");
    auto message = certificate_verify_message(transcript_through_certificate);
    return nxt::crypto::ecdsa_p256_sha256_verify(
        p256_public_key, message, certificate_verify.signature);
}

} // namespace nxt::tls
