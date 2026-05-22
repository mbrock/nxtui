#include "nxt/rt/task.hpp"
#include <nxt/crypto.hpp>
#include <nxt/rt/buffers.hpp>
#include <nxt/rt/cares.hpp>
#include <nxt/rt/http.hpp>
#include <nxt/unique-fd.hpp>

#if defined(__linux__)
#include <nxt/rt/uring_wand.hpp>
#else
#include <nxt/rt/kqueue_wand.hpp>
#endif

#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/evp.h>
#include <openssl/nid.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

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

void set_close_on_exec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

nxt::rt::task<nxt::unique_fd> connect_address(
    nxt::rt::resolved_address address)
{
    auto fd = nxt::unique_fd{::socket(
        address.family,
        address.socktype == 0 ? SOCK_STREAM : address.socktype,
        address.protocol)};
    if (fd.get() < 0)
        throw nxt::rt::runtime_error{
            "socket: " + std::string{std::generic_category().message(errno)}};

    set_close_on_exec(fd.get());
    co_await nxt::rt::op::connect::from(
        fd.get(), address.sockaddr_ptr(), address.address_size);
    co_return std::move(fd);
}

nxt::rt::task<nxt::unique_fd>
connect_tcp(std::string host, std::string port)
{
    auto resolver = nxt::rt::cares_resolver{};
    auto addresses = co_await resolver.getaddrinfo(host, port);
    if (addresses.empty())
        throw nxt::rt::runtime_error{"no addresses resolved for " + host};

    co_return co_await nxt::rt::wait_any_range(
        addresses | std::views::transform([](auto const & address) {
            return connect_address(address);
        }));
}

using bytes = std::vector<std::byte>;

struct tls13_client_hello
{
    bytes record;
    bytes handshake;
    bytes legacy_session_id;
    nxt::crypto::x25519_key_pair key_pair;
};

void put_u8(bytes & out, std::uint8_t value)
{
    out.push_back(static_cast<std::byte>(value));
}

void put_u16(bytes & out, std::uint16_t value)
{
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value));
}

void put_u24(bytes & out, std::uint32_t value)
{
    put_u8(out, static_cast<std::uint8_t>(value >> 16));
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value));
}

void put_bytes(bytes & out, std::span<const std::byte> input)
{
    out.insert(out.end(), input.begin(), input.end());
}

void put_text(bytes & out, std::string_view text)
{
    auto input = std::as_bytes(std::span{text});
    put_bytes(out, input);
}

void put_extension(bytes & out, std::uint16_t type, bytes body)
{
    put_u16(out, type);
    put_u16(out, static_cast<std::uint16_t>(body.size()));
    put_bytes(out, body);
}

tls13_client_hello make_tls13_client_hello(std::string_view host)
{
    auto x25519 = nxt::crypto::x25519_keygen();
    auto random = nxt::crypto::random(32);
    auto session_id = nxt::crypto::random(32);

    auto extensions = bytes{};
    {
        auto names = bytes{};
        put_u8(names, 0); // host_name
        put_u16(names, static_cast<std::uint16_t>(host.size()));
        put_text(names, host);

        auto body = bytes{};
        put_u16(body, static_cast<std::uint16_t>(names.size()));
        put_bytes(body, names);
        put_extension(extensions, 0, std::move(body)); // server_name
    }
    {
        auto body = bytes{};
        put_u8(body, 2);
        put_u16(body, 0x0304); // TLS 1.3
        put_extension(extensions, 43, std::move(body)); // supported_versions
    }
    {
        auto body = bytes{};
        put_u16(body, 2);
        put_u16(body, 0x001d); // x25519
        put_extension(extensions, 10, std::move(body)); // supported_groups
    }
    {
        auto body = bytes{};
        put_u16(body, 8);
        put_u16(body, 0x0403); // ecdsa_secp256r1_sha256
        put_u16(body, 0x0804); // rsa_pss_rsae_sha256
        put_u16(body, 0x0805); // rsa_pss_rsae_sha384
        put_u16(body, 0x0401); // rsa_pkcs1_sha256
        put_extension(extensions, 13, std::move(body)); // signature_algorithms
    }
    {
        auto share = bytes{};
        put_u16(share, 0x001d); // x25519
        put_u16(share, static_cast<std::uint16_t>(x25519.public_key.size()));
        put_bytes(share, x25519.public_key);

        auto body = bytes{};
        put_u16(body, static_cast<std::uint16_t>(share.size()));
        put_bytes(body, share);
        put_extension(extensions, 51, std::move(body)); // key_share
    }
    {
        auto body = bytes{};
        put_u16(body, 9);
        put_u8(body, 8);
        put_text(body, "http/1.1");
        put_extension(extensions, 16, std::move(body)); // application_layer_protocol_negotiation
    }

    auto hello = bytes{};
    put_u16(hello, 0x0303); // ClientHello.legacy_version
    put_bytes(hello, random);
    put_u8(hello, static_cast<std::uint8_t>(session_id.size()));
    put_bytes(hello, session_id);

    put_u16(hello, 2);
    put_u16(hello, 0x1301); // TLS_AES_128_GCM_SHA256

    put_u8(hello, 1);
    put_u8(hello, 0); // legacy null compression

    put_u16(hello, static_cast<std::uint16_t>(extensions.size()));
    put_bytes(hello, extensions);

    auto handshake = bytes{};
    put_u8(handshake, 1); // client_hello
    put_u24(handshake, static_cast<std::uint32_t>(hello.size()));
    put_bytes(handshake, hello);

    auto record = bytes{};
    put_u8(record, 22); // handshake
    put_u16(record, 0x0301); // legacy record version for ClientHello
    put_u16(record, static_cast<std::uint16_t>(handshake.size()));
    put_bytes(record, handshake);
    return tls13_client_hello{
        .record = std::move(record),
        .handshake = std::move(handshake),
        .legacy_session_id = std::move(session_id),
        .key_pair = x25519,
    };
}

void dump_hex(std::span<const std::byte> bytes)
{
    auto flags = std::cerr.flags();
    auto fill = std::cerr.fill();
    for (auto offset = std::size_t{0}; offset < bytes.size(); offset += 16) {
        std::cerr << std::setw(4) << std::setfill('0') << std::hex << offset
                  << "  ";
        auto line = bytes.subspan(offset, std::min<std::size_t>(
                                              16, bytes.size() - offset));
        for (auto byte : line)
            std::cerr << std::setw(2) << static_cast<unsigned>(
                             std::to_integer<unsigned char>(byte))
                      << ' ';
        std::cerr << '\n';
    }
    std::cerr.flags(flags);
    std::cerr.fill(fill);
}

std::uint16_t parse_u16(std::span<const std::byte> input)
{
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input[0]) << 8)
        | std::to_integer<std::uint16_t>(input[1]));
}

std::uint32_t parse_u24(std::span<const std::byte> input)
{
    return (std::to_integer<std::uint32_t>(input[0]) << 16)
        | (std::to_integer<std::uint32_t>(input[1]) << 8)
        | std::to_integer<std::uint32_t>(input[2]);
}

class byte_cursor
{
public:
    explicit byte_cursor(std::span<const std::byte> input)
        : input_(input)
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return input_.empty();
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return input_.size();
    }

    std::uint8_t take_u8()
    {
        return std::to_integer<std::uint8_t>(take(1)[0]);
    }

    std::uint16_t take_u16()
    {
        return parse_u16(take(2));
    }

    std::uint32_t take_u24()
    {
        return parse_u24(take(3));
    }

    std::span<const std::byte> take(std::size_t n)
    {
        if (n > input_.size())
            throw nxt::rt::runtime_error{"truncated TLS message"};
        auto out = input_.first(n);
        input_ = input_.subspan(n);
        return out;
    }

private:
    std::span<const std::byte> input_;
};

template<typename Reader>
nxt::rt::task<std::uint8_t> read_u8(Reader & reader)
{
    auto bytes = co_await reader.take(1);
    co_return std::to_integer<std::uint8_t>(bytes[0]);
}

template<typename Reader>
nxt::rt::task<std::uint16_t> read_u16(Reader & reader)
{
    co_return parse_u16(co_await reader.take(2));
}

template<typename Reader>
nxt::rt::task<std::uint32_t> read_u24(Reader & reader)
{
    co_return parse_u24(co_await reader.take(3));
}

std::string_view tls_record_type_name(std::uint8_t type)
{
    switch (type) {
    case 20:
        return "change_cipher_spec";
    case 21:
        return "alert";
    case 22:
        return "handshake";
    case 23:
        return "application_data";
    default:
        return "unknown";
    }
}

std::string_view tls_handshake_type_name(std::uint8_t type)
{
    switch (type) {
    case 1:
        return "client_hello";
    case 2:
        return "server_hello";
    case 8:
        return "encrypted_extensions";
    case 11:
        return "certificate";
    case 15:
        return "certificate_verify";
    case 20:
        return "finished";
    default:
        return "unknown";
    }
}

struct tls_record
{
    std::uint8_t type = 0;
    std::uint16_t version = 0;
    bytes payload;
};

struct tls13_server_hello
{
    bytes handshake;
    std::array<std::byte, 32> random{};
    bytes legacy_session_id;
    std::uint16_t cipher_suite = 0;
    std::array<std::byte, nxt::crypto::x25519_key_len> key_share{};
};

template<typename Reader>
nxt::rt::task<tls_record> read_tls_record(Reader & reader)
{
    auto type = co_await read_u8(reader);
    auto version = co_await read_u16(reader);
    auto length = co_await read_u16(reader);
    auto payload = co_await reader.take(length);
    co_return tls_record{
        .type = type,
        .version = version,
        .payload = bytes{payload.begin(), payload.end()},
    };
}

void require_tls(bool ok, const char * message)
{
    if (!ok)
        throw nxt::rt::runtime_error{message};
}

tls13_server_hello parse_tls13_server_hello(tls_record const & record)
{
    require_tls(record.type == 22, "expected a TLS handshake record");
    require_tls(record.version == 0x0303, "expected legacy TLS 1.2 record version");

    auto handshake = byte_cursor{record.payload};
    auto handshake_type = handshake.take_u8();
    auto handshake_length = handshake.take_u24();
    require_tls(handshake_type == 2, "expected server_hello");
    require_tls(
        handshake_length == handshake.remaining(),
        "server_hello length does not match record payload");

    auto server_hello = tls13_server_hello{};
    server_hello.handshake = record.payload;
    auto body = byte_cursor{handshake.take(handshake_length)};
    require_tls(
        body.take_u16() == 0x0303,
        "expected ServerHello.legacy_version 0x0303");

    auto random = body.take(server_hello.random.size());
    std::ranges::copy(random, server_hello.random.begin());

    auto session_id_len = body.take_u8();
    auto session_id = body.take(session_id_len);
    server_hello.legacy_session_id = bytes{session_id.begin(), session_id.end()};

    server_hello.cipher_suite = body.take_u16();
    require_tls(
        server_hello.cipher_suite == 0x1301,
        "server selected an unsupported cipher suite");

    require_tls(
        body.take_u8() == 0,
        "server selected an unsupported compression method");

    auto extensions_len = body.take_u16();
    auto extensions = byte_cursor{body.take(extensions_len)};
    require_tls(body.empty(), "unexpected bytes after ServerHello extensions");

    auto saw_supported_versions = false;
    auto saw_key_share = false;
    while (!extensions.empty()) {
        auto type = extensions.take_u16();
        auto length = extensions.take_u16();
        auto extension = byte_cursor{extensions.take(length)};

        switch (type) {
        case 43: { // supported_versions
            require_tls(
                extension.take_u16() == 0x0304,
                "server selected an unsupported TLS version");
            require_tls(
                extension.empty(),
                "unexpected bytes in supported_versions extension");
            saw_supported_versions = true;
            break;
        }
        case 51: { // key_share
            require_tls(
                extension.take_u16() == 0x001d,
                "server selected a non-X25519 key share");
            auto key_len = extension.take_u16();
            require_tls(
                key_len == server_hello.key_share.size(),
                "server X25519 key share has the wrong length");
            auto key = extension.take(key_len);
            std::ranges::copy(key, server_hello.key_share.begin());
            require_tls(
                extension.empty(),
                "unexpected bytes in key_share extension");
            saw_key_share = true;
            break;
        }
        default:
            break;
        }
    }

    require_tls(saw_supported_versions, "ServerHello omitted supported_versions");
    require_tls(saw_key_share, "ServerHello omitted key_share");
    return server_hello;
}

void describe_tls_record(std::size_t index, tls_record const & record)
{
    std::cerr << "record " << index << ": " << tls_record_type_name(record.type)
              << " type=" << unsigned{record.type} << " version=0x" << std::hex
              << record.version << std::dec
              << " length=" << record.payload.size() << '\n';

    if (record.type == 22 && record.payload.size() >= 4) {
        auto handshake_type =
            std::to_integer<std::uint8_t>(record.payload[0]);
        auto handshake_length = parse_u24(std::span{record.payload}.subspan(1, 3));
        std::cerr << "  handshake: " << tls_handshake_type_name(handshake_type)
                  << " type=" << unsigned{handshake_type}
                  << " length=" << handshake_length << '\n';
    }
}

void describe_server_hello(tls13_server_hello const & server_hello)
{
    std::cerr << "server hello: TLS 1.3, TLS_AES_128_GCM_SHA256, x25519"
              << "\n";
    std::cerr << "  legacy session id: "
              << server_hello.legacy_session_id.size() << " bytes\n";
}

void describe_handshake_messages(std::span<const std::byte> bytes)
{
    auto cursor = byte_cursor{bytes};
    while (!cursor.empty()) {
        if (cursor.remaining() < 4) {
            std::cerr << "  trailing partial handshake header: "
                      << cursor.remaining() << " bytes\n";
            return;
        }

        auto type = cursor.take_u8();
        auto length = cursor.take_u24();
        if (length > cursor.remaining()) {
            std::cerr << "  handshake: " << tls_handshake_type_name(type)
                      << " type=" << unsigned{type} << " length=" << length
                      << " partial, have " << cursor.remaining() << " bytes\n";
            return;
        }

        cursor.take(length);
        std::cerr << "  handshake: " << tls_handshake_type_name(type)
                  << " type=" << unsigned{type} << " length=" << length
                  << '\n';
    }
}

std::vector<bytes> split_handshake_messages(std::span<const std::byte> input)
{
    auto cursor = byte_cursor{input};
    auto out = std::vector<bytes>{};
    while (!cursor.empty()) {
        auto before = cursor.remaining();
        cursor.take_u8();
        auto length = cursor.take_u24();
        cursor.take(length);
        auto consumed = before - cursor.remaining();
        auto message = input.first(consumed);
        out.emplace_back(message.begin(), message.end());
        input = input.subspan(consumed);
    }
    return out;
}

bytes join_bytes(std::span<const std::byte> left, std::span<const std::byte> right)
{
    auto out = bytes{};
    out.reserve(left.size() + right.size());
    put_bytes(out, left);
    put_bytes(out, right);
    return out;
}

bytes hkdf_expand_label(
    std::span<const std::byte> secret,
    std::string_view label,
    std::span<const std::byte> context,
    std::size_t length)
{
    auto hkdf_label = bytes{};
    put_u16(hkdf_label, static_cast<std::uint16_t>(length));

    auto full_label = std::string{"tls13 "};
    full_label += label;
    put_u8(hkdf_label, static_cast<std::uint8_t>(full_label.size()));
    put_text(hkdf_label, full_label);

    put_u8(hkdf_label, static_cast<std::uint8_t>(context.size()));
    put_bytes(hkdf_label, context);

    return nxt::crypto::hkdf_expand_sha256(secret, hkdf_label, length);
}

std::array<std::byte, nxt::crypto::sha256_len> derive_secret(
    std::span<const std::byte> secret,
    std::string_view label,
    std::span<const std::byte> messages)
{
    auto transcript_hash = nxt::crypto::sha256(messages);
    auto out = hkdf_expand_label(secret, label, transcript_hash, transcript_hash.size());
    auto array = std::array<std::byte, nxt::crypto::sha256_len>{};
    std::ranges::copy(out, array.begin());
    return array;
}

struct tls13_read_keys
{
    std::array<std::byte, nxt::crypto::sha256_len> traffic_secret{};
    bytes key;
    bytes iv;
    std::uint64_t sequence = 0;
};

tls13_read_keys derive_server_handshake_read_keys(
    std::span<const std::byte> shared_secret,
    std::span<const std::byte> transcript)
{
    auto zero = bytes(nxt::crypto::sha256_len);
    auto early_secret = nxt::crypto::hkdf_extract_sha256(zero, zero);
    auto derived_secret = derive_secret(early_secret, "derived", {});
    auto handshake_secret =
        nxt::crypto::hkdf_extract_sha256(derived_secret, shared_secret);
    auto server_traffic_secret =
        derive_secret(handshake_secret, "s hs traffic", transcript);

    return tls13_read_keys{
        .traffic_secret = server_traffic_secret,
        .key = hkdf_expand_label(
            server_traffic_secret, "key", {}, nxt::crypto::aes128_key_len),
        .iv = hkdf_expand_label(
            server_traffic_secret, "iv", {}, nxt::crypto::aes_gcm_nonce_len),
        .sequence = 0,
    };
}

bytes tls13_record_aad(tls_record const & record)
{
    auto aad = bytes{};
    put_u8(aad, record.type);
    put_u16(aad, record.version);
    put_u16(aad, static_cast<std::uint16_t>(record.payload.size()));
    return aad;
}

bytes tls13_record_nonce(std::span<const std::byte> iv, std::uint64_t sequence)
{
    auto nonce = bytes{iv.begin(), iv.end()};
    for (auto i = 0; i < 8; ++i) {
        auto shift = static_cast<unsigned>((7 - i) * 8);
        nonce[nonce.size() - 8 + i] ^= static_cast<std::byte>(sequence >> shift);
    }
    return nonce;
}

struct tls13_plaintext
{
    bytes content;
    std::uint8_t inner_type = 0;
};

struct tls13_certificate
{
    bytes leaf_der;
};

struct tls13_certificate_verify
{
    std::uint16_t scheme = 0;
    bytes signature;
};

tls13_plaintext open_tls13_record(
    tls13_read_keys & keys,
    tls_record const & record)
{
    require_tls(record.type == 23, "expected encrypted application_data record");
    auto aad = tls13_record_aad(record);
    auto nonce = tls13_record_nonce(keys.iv, keys.sequence++);
    auto plaintext =
        nxt::crypto::aes128gcm_open(keys.key, nonce, aad, record.payload);
    require_tls(plaintext.has_value(), "failed to decrypt TLS record");

    while (!plaintext->empty() && plaintext->back() == std::byte{0})
        plaintext->pop_back();
    require_tls(!plaintext->empty(), "decrypted TLS record has no inner type");

    auto inner_type = std::to_integer<std::uint8_t>(plaintext->back());
    plaintext->pop_back();
    return tls13_plaintext{
        .content = std::move(*plaintext),
        .inner_type = inner_type,
    };
}

tls13_certificate parse_tls13_certificate(std::span<const std::byte> message)
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

    auto cert = entries.take(entries.take_u24());
    auto extensions_len = entries.take_u16();
    entries.take(extensions_len);

    return tls13_certificate{
        .leaf_der = bytes{cert.begin(), cert.end()},
    };
}

tls13_certificate_verify parse_tls13_certificate_verify(
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

bytes extract_p256_public_key_from_certificate(std::span<const std::byte> der)
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

bytes certificate_verify_message(std::span<const std::byte> transcript)
{
    auto out = bytes(64, std::byte{0x20});
    put_text(out, "TLS 1.3, server CertificateVerify");
    put_u8(out, 0);
    auto hash = nxt::crypto::sha256(transcript);
    put_bytes(out, hash);
    return out;
}

bool verify_certificate_verify(
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

nxt::rt::task<void> probe_tls13(nxt::rt::http::url url)
{
    auto socket = co_await connect_tcp(url.host, url.port);
    auto hello = make_tls13_client_hello(url.host);

    std::cerr << "sending TLS 1.3 ClientHello (" << hello.record.size()
              << " bytes)\n";
    auto socket_output = nxt::rt::byte_writer<nxt::rt::socket_sink>{
        nxt::rt::socket_sink{socket.get()},
        4096,
    };
    co_await socket_output.write_all(hello.record);

    auto source = nxt::rt::socket_source{socket.get()};
    auto input_storage = std::vector<std::byte>(18 * 1024);
    auto reader = nxt::rt::byte_reader<nxt::rt::socket_source>{
        source,
        std::span{input_storage},
    };

    auto record = co_await read_tls_record(reader);
    describe_tls_record(0, record);
    auto server_hello = parse_tls13_server_hello(record);
    describe_server_hello(server_hello);

    auto shared_secret =
        nxt::crypto::x25519_dh(hello.key_pair.secret_key, server_hello.key_share);
    require_tls(shared_secret.has_value(), "X25519 shared secret failed");
    std::cerr << "computed X25519 shared secret: " << shared_secret->size()
              << " bytes\n";

    auto index = std::size_t{1};
    auto transcript = join_bytes(hello.handshake, server_hello.handshake);
    auto server_read_keys =
        derive_server_handshake_read_keys(*shared_secret, transcript);
    std::cerr << "derived server handshake AES-128-GCM key and IV\n";

    record = co_await read_tls_record(reader);
    describe_tls_record(index, record);
    if (record.type == 20) {
        dump_hex(record.payload);
        ++index;
        record = co_await read_tls_record(reader);
        describe_tls_record(index, record);
    }

    auto leaf_public_key = std::optional<bytes>{};
    while (true) {
        auto plaintext = open_tls13_record(server_read_keys, record);
        std::cerr << "decrypted inner record: "
                  << tls_record_type_name(plaintext.inner_type)
                  << " type=" << unsigned{plaintext.inner_type}
                  << " length=" << plaintext.content.size() << '\n';
        if (plaintext.inner_type == 22)
            describe_handshake_messages(plaintext.content);
        if (plaintext.content.size() <= 256)
            dump_hex(plaintext.content);

        if (plaintext.inner_type == 22) {
            for (auto const & message : split_handshake_messages(plaintext.content)) {
                auto type = std::to_integer<std::uint8_t>(message[0]);
                if (type == 11) {
                    auto cert = parse_tls13_certificate(message);
                    leaf_public_key =
                        extract_p256_public_key_from_certificate(cert.leaf_der);
                    std::cerr << "extracted leaf P-256 public key: "
                              << leaf_public_key->size() << " bytes\n";
                } else if (type == 15) {
                    require_tls(
                        leaf_public_key.has_value(),
                        "certificate_verify arrived before certificate public key");
                    auto cert_verify = parse_tls13_certificate_verify(message);
                    auto ok = verify_certificate_verify(
                        *leaf_public_key, transcript, cert_verify);
                    require_tls(ok, "CertificateVerify signature failed");
                    std::cerr << "verified CertificateVerify signature\n";
                }
                put_bytes(transcript, message);
            }
        }

        if (reader.buffered_size() == 0)
            break;
        ++index;
        record = co_await read_tls_record(reader);
        describe_tls_record(index, record);
    }
}

nxt::rt::task<void> fetch(nxt::rt::http::url url)
{
    if (url.tls)
        co_return co_await probe_tls13(std::move(url));

    auto socket = co_await connect_tcp(url.host, url.port);
    auto request = nxt::rt::http::request{
        .method = "GET",
        .target = url.target,
        .host = nxt::rt::http::host_header(url),
        .headers =
            {
                {"User-Agent", "nxt-ng-http-demo/0"},
                {"Accept", "*/*"},
            },
        .body = {},
    };

    auto socket_output = nxt::rt::byte_writer<nxt::rt::socket_sink>{
        nxt::rt::socket_sink{socket.get()},
        4096,
    };
    co_await socket_output.write_all(nxt::rt::http::serialize(request));

    auto source = nxt::rt::socket_source{socket.get()};
    auto input_storage = std::vector<std::byte>(64 * 1024);
    auto reader = nxt::rt::byte_reader<nxt::rt::socket_source>{
        source,
        std::span{input_storage},
    };

    auto head = co_await nxt::rt::http::read_response_head(reader);
    std::cerr << head.version << ' ' << head.status << ' ' << head.reason
              << "\n";
    for (auto const & header : head.headers)
        std::cerr << header.name << ": " << header.value << "\n";
    std::cerr << "\n";

    auto stdout_writer = nxt::rt::standard_output_writer(64 * 1024);
    auto body = nxt::rt::http::read_response_body(reader, head);
    while (auto chunk = co_await body.next())
        co_await stdout_writer.write(*chunk);
    co_await stdout_writer.flush();
}

} // namespace

int main(int argc, char ** argv)
try {
    auto url = nxt::rt::http::parse_url(
        argc > 1 ? std::string_view{argv[1]}
                 : std::string_view{"https://less.rest/"});

#if defined(__linux__)
    nxt::rt::run([url = std::move(url)]() mutable {
        return fetch(std::move(url));
    });
#elif NXT_RT_HAS_KQUEUE
    nxt::rt::run_with_kqueue([url = std::move(url)]() mutable {
        return fetch(std::move(url));
    });
#else
    static_assert(NXT_RT_HAS_KQUEUE, "ng http demo needs a runtime wand");
#endif

    return 0;
} catch (std::exception const & error) {
    std::cerr << "ng-http-client-demo: " << error.what() << '\n';
    return 1;
}
