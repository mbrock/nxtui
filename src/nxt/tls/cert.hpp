#pragma once

#include <nxt/tls.hpp>

namespace nxt::tls {

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

enum class leaf_public_key_kind
{
    p256,
    rsa,
};

struct leaf_public_key
{
    leaf_public_key_kind kind = leaf_public_key_kind::p256;
    bytes ec_point;
    bytes spki_der;
};

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
        require_tls(offset_ < input_.size(), "truncated DER value");
        return input_[offset_++];
    }

    std::span<const std::byte> take(std::size_t n)
    {
        require_tls(n <= input_.size() - offset_, "truncated DER value");
        auto out = input_.subspan(offset_, n);
        offset_ += n;
        return out;
    }

    std::span<const std::byte> take_tlv(std::byte expected_tag)
    {
        auto start = offset_;
        auto tag = take_u8();
        require_tls(tag == expected_tag, "unexpected DER tag");
        auto len = take_length();
        take(len);
        return input_.subspan(start, offset_ - start);
    }

    std::span<const std::byte> take_value(std::byte expected_tag)
    {
        auto tag = take_u8();
        require_tls(tag == expected_tag, "unexpected DER tag");
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
        require_tls(octets != 0, "indefinite DER length is not allowed");
        require_tls(octets <= sizeof(std::size_t), "DER length is too large");
        require_tls(
            offset_ + octets <= input_.size(), "truncated DER length");

        auto out = std::size_t{0};
        for (std::size_t i = 0; i < octets; i++)
            out = (out << 8) | std::to_integer<unsigned char>(take_u8());
        require_tls(out >= 128, "non-minimal DER length");
        return out;
    }

    std::span<const std::byte> input_;
    std::size_t offset_ = 0;
};

inline bool der_equal(
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

inline std::span<const std::byte>
certificate_spki(std::span<const std::byte> der)
{
    auto cert = der_cursor{der_cursor{der}.take_value(std::byte{0x30})};
    auto tbs = der_cursor{cert.take_value(std::byte{0x30})};

    if (!tbs.empty() && tbs.remaining().front() == std::byte{0xa0})
        tbs.take_tlv(std::byte{0xa0}); // version

    tbs.take_tlv(std::byte{0x02}); // serialNumber
    tbs.take_tlv(std::byte{0x30}); // signature
    tbs.take_tlv(std::byte{0x30}); // issuer
    tbs.take_tlv(std::byte{0x30}); // validity
    tbs.take_tlv(std::byte{0x30}); // subject
    return tbs.take_tlv(std::byte{0x30}); // subjectPublicKeyInfo
}

inline tls13_certificate
parse_tls13_certificate(std::span<const std::byte> message)
{
    auto cursor = byte_cursor{message};
    require_tls(cursor.take_u8() == 11, "expected certificate message");
    auto length = cursor.take_u24();
    auto body = byte_cursor{cursor.take(length)};
    require_tls(
        cursor.empty(), "unexpected bytes after certificate message");

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

inline tls13_certificate_verify
parse_tls13_certificate_verify(std::span<const std::byte> message)
{
    auto cursor = byte_cursor{message};
    require_tls(
        cursor.take_u8() == 15, "expected certificate_verify message");
    auto length = cursor.take_u24();
    auto body = byte_cursor{cursor.take(length)};
    require_tls(
        cursor.empty(),
        "unexpected bytes after certificate_verify message");

    auto scheme = body.take_u16();
    auto signature = body.take(body.take_u16());
    require_tls(
        body.empty(),
        "unexpected bytes after certificate_verify signature");
    return tls13_certificate_verify{
        .scheme = scheme,
        .signature = bytes{signature.begin(), signature.end()},
    };
}

inline leaf_public_key
extract_leaf_public_key(std::span<const std::byte> der)
{
    auto out = leaf_public_key{};
    auto spki = certificate_spki(der);
    out.spki_der.assign(spki.begin(), spki.end());

    auto spki_body = der_cursor{der_cursor{spki}.take_value(std::byte{0x30})};
    auto algorithm =
        der_cursor{spki_body.take_value(std::byte{0x30})};
    auto algorithm_oid = algorithm.take_value(std::byte{0x06});
    auto public_key = der_cursor{spki_body.take_value(std::byte{0x03})};
    auto unused_bits = public_key.take_u8();
    require_tls(unused_bits == std::byte{0}, "unsupported DER BIT STRING");

    if (der_equal(
            algorithm_oid,
            {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01})) {
        out.kind = leaf_public_key_kind::rsa;
        return out;
    }

    require_tls(
        der_equal(algorithm_oid, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01}),
        "leaf certificate public key is not EC or RSA");
    require_tls(
        !algorithm.empty(), "EC public key parameters are missing");
    auto curve_oid = algorithm.take_value(std::byte{0x06});
    require_tls(
        der_equal(curve_oid, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07}),
        "leaf certificate public key is not P-256");
    require_tls(algorithm.empty(), "unexpected EC public key parameters");

    auto point = public_key.remaining();
    require_tls(
        point.size() == 65 && point.front() == std::byte{0x04},
        "leaf certificate P-256 public key is not uncompressed");
    out.kind = leaf_public_key_kind::p256;
    out.ec_point.assign(point.begin(), point.end());
    return out;
}

inline bytes
certificate_verify_message(std::span<const std::byte> transcript)
{
    auto out = bytes(64, std::byte{0x20});
    put_text(out, "TLS 1.3, server CertificateVerify");
    put_u8(out, 0);
    auto hash = nxt::crypto::sha256(transcript);
    put_bytes(out, hash);
    return out;
}

inline bool verify_certificate_verify(
    leaf_public_key const & leaf,
    std::span<const std::byte> transcript_through_certificate,
    tls13_certificate_verify const & certificate_verify)
{
    auto message =
        certificate_verify_message(transcript_through_certificate);
    switch (certificate_verify.scheme) {
    case 0x0403:
        require_tls(
            leaf.kind == leaf_public_key_kind::p256,
            "server used ECDSA CertificateVerify with a non-P-256 leaf");
        return nxt::crypto::ecdsa_p256_sha256_verify(
            leaf.ec_point, message, certificate_verify.signature);
    case 0x0804:
        require_tls(
            leaf.kind == leaf_public_key_kind::rsa,
            "server used RSA-PSS CertificateVerify with a non-RSA leaf");
        return nxt::crypto::rsa_pss_verify(
            leaf.spki_der, message, certificate_verify.signature, 256);
    case 0x0805:
        require_tls(
            leaf.kind == leaf_public_key_kind::rsa,
            "server used RSA-PSS CertificateVerify with a non-RSA leaf");
        return nxt::crypto::rsa_pss_verify(
            leaf.spki_der, message, certificate_verify.signature, 384);
    case 0x0806:
        require_tls(
            leaf.kind == leaf_public_key_kind::rsa,
            "server used RSA-PSS CertificateVerify with a non-RSA leaf");
        return nxt::crypto::rsa_pss_verify(
            leaf.spki_der, message, certificate_verify.signature, 512);
    default:
        require_tls(
            false,
            "server used an unsupported CertificateVerify signature scheme");
    }
    return false;
}

} // namespace nxt::tls
