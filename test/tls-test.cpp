#include <nxt/tls/cert.hpp>
#include <nxtrt/deck.hpp>
#include <nxtrt/tls.hpp>

#include "test.hpp"

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <memory>
#include <unistd.h>

namespace nxt::test {
namespace {

using key_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using cert_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

void fixture_check(bool ok)
{
    if (!ok)
        throw std::runtime_error{"TLS fixture construction failed"};
}

key_ptr certificate_key()
{
    auto context =
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>{
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free};
    fixture_check(context != nullptr);
    fixture_check(EVP_PKEY_keygen_init(context.get()) == 1);
    fixture_check(
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(
            context.get(), NID_X9_62_prime256v1)
        == 1);
    EVP_PKEY * key = nullptr;
    fixture_check(EVP_PKEY_keygen(context.get(), &key) == 1);
    return key_ptr{key, EVP_PKEY_free};
}

void certificate_extension(X509 * cert, int nid, const char * value)
{
    auto old = X509_get_ext_by_NID(cert, nid, -1);
    if (old >= 0)
        X509_EXTENSION_free(X509_delete_ext(cert, old));
    auto extension =
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>{
            X509V3_EXT_conf_nid(
                nullptr, nullptr, nid, const_cast<char *>(value)),
            X509_EXTENSION_free};
    fixture_check(extension != nullptr);
    fixture_check(X509_add_ext(cert, extension.get(), -1) == 1);
}

cert_ptr make_certificate(
    EVP_PKEY * key,
    X509 * issuer,
    EVP_PKEY * signer,
    const char * name,
    bool ca)
{
    auto cert = cert_ptr{X509_new(), X509_free};
    fixture_check(cert != nullptr);
    fixture_check(X509_set_version(cert.get(), 2) == 1);
    fixture_check(
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), ca ? 1 : 2)
        == 1);
    fixture_check(
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) != nullptr);
    fixture_check(
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) != nullptr);
    fixture_check(X509_set_pubkey(cert.get(), key) == 1);
    auto subject = X509_get_subject_name(cert.get());
    fixture_check(
        X509_NAME_add_entry_by_txt(
            subject,
            "CN",
            MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>(name),
            -1,
            -1,
            0)
        == 1);
    fixture_check(
        X509_set_issuer_name(
            cert.get(), issuer ? X509_get_subject_name(issuer) : subject)
        == 1);
    certificate_extension(
        cert.get(),
        NID_basic_constraints,
        ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
    certificate_extension(
        cert.get(),
        NID_key_usage,
        ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
    if (!ca) {
        certificate_extension(cert.get(), NID_ext_key_usage, "serverAuth");
        certificate_extension(
            cert.get(),
            NID_subject_alt_name,
            "DNS:service.example,DNS:*.wild.example,IP:127.0.0.1,IP:::1");
    }
    fixture_check(X509_sign(cert.get(), signer, EVP_sha256()) > 0);
    return cert;
}

nxt::tls::bytes certificate_der(X509 * cert)
{
    auto size = i2d_X509(cert, nullptr);
    fixture_check(size > 0);
    auto out = nxt::tls::bytes(size);
    auto cursor = reinterpret_cast<unsigned char *>(out.data());
    fixture_check(i2d_X509(cert, &cursor) == size);
    return out;
}

struct certificate_fixture
{
    key_ptr root_key = certificate_key();
    key_ptr intermediate_key = certificate_key();
    key_ptr leaf_key = certificate_key();
    cert_ptr root = make_certificate(
        root_key.get(), nullptr, root_key.get(), "root", true);
    cert_ptr intermediate = make_certificate(
        intermediate_key.get(),
        root.get(),
        root_key.get(),
        "intermediate",
        true);
    // CN intentionally differs from the SAN to detect CN fallback.
    cert_ptr leaf = make_certificate(
        leaf_key.get(),
        intermediate.get(),
        intermediate_key.get(),
        "cn.example",
        false);
    std::array<char, 32> path{};

    certificate_fixture()
    {
        constexpr auto pattern = "/tmp/nxt-tls-ca-XXXXXX";
        std::copy_n(
            pattern,
            std::char_traits<char>::length(pattern) + 1,
            path.begin());
        auto fd = mkstemp(path.data());
        fixture_check(fd >= 0);
        auto file = fdopen(fd, "w");
        if (!file) {
            close(fd);
            unlink(path.data());
            fixture_check(false);
        }
        auto ok = PEM_write_X509(file, root.get()) == 1;
        ok = fclose(file) == 0 && ok;
        fixture_check(ok);
    }

    ~certificate_fixture()
    {
        unlink(path.data());
    }

    nxt::tls::tls13_certificate chain()
    {
        auto der = certificate_der(leaf.get());
        return {
            .leaf_der = der,
            .chain_der = {der, certificate_der(intermediate.get())}};
    }

    void sign_leaf()
    {
        fixture_check(
            X509_sign(leaf.get(), intermediate_key.get(), EVP_sha256())
            > 0);
    }
};

std::string certificate_failure(
    nxt::tls::tls13_certificate const & chain,
    std::string_view host,
    std::string_view ca_file)
{
    try {
        nxt::tls::verify_server_certificate(chain, host, ca_file);
        return {};
    } catch (nxtrt::runtime_error const & error) {
        return error.what();
    }
}

// A peer that knows the handshake keys can forge Finished without knowing
// any certificate private key. Exercise that attack against the real
// client.
class handshake_peer final : public nxtrt::bytesink
{
public:
    handshake_peer(
        nxt::tls::bytes & reply,
        certificate_fixture & fixture,
        std::vector<int> types,
        bool corrupt_signature = false)
        : nxtrt::bytesink(std::size_t{0})
        , reply_(reply)
        , fixture_(fixture)
        , types_(std::move(types))
        , corrupt_signature_(corrupt_signature)
    {
    }

    int writes = 0;

private:
    nxtrt::hope<std::size_t>
    drain_more(value_chunk_view chunks, std::size_t splat) override
    {
        using namespace nxt::tls;
        fixture_check(splat == 1);
        auto sent = bytes{};
        for (auto chunk : chunks)
            put_bytes(sent, chunk);
        if (++writes != 1)
            return nxtrt::hope<std::size_t>::ready(sent.size());

        auto client_hello = std::span{sent}.subspan(5);
        auto body = byte_cursor{client_hello.subspan(4)};
        body.take(34); // version and random
        auto session_id = body.take(body.take_u8());
        body.take(body.take_u16()); // cipher suites
        body.take(body.take_u8());  // compression
        auto extensions = byte_cursor{body.take(body.take_u16())};
        auto client_share = bytes{};
        while (!extensions.empty()) {
            auto type = extensions.take_u16();
            auto value =
                byte_cursor{extensions.take(extensions.take_u16())};
            if (type == 51) {
                value.take_u16(); // list length
                fixture_check(value.take_u16() == 0x001d);
                auto share = value.take(value.take_u16());
                client_share.assign(share.begin(), share.end());
            }
        }
        auto key = nxt::crypto::x25519_keygen();
        auto server_extensions = bytes{};
        put_extension(
            server_extensions, 43, bytes{std::byte{3}, std::byte{4}});
        auto share = bytes{};
        put_u16(share, 0x001d);
        put_u16(share, 32);
        put_bytes(share, key.public_key);
        put_extension(server_extensions, 51, share);
        auto hello_body = bytes{};
        put_u16(hello_body, 0x0303);
        put_bytes(hello_body, nxt::crypto::random(32));
        put_u8(hello_body, session_id.size());
        put_bytes(hello_body, session_id);
        put_u16(hello_body, 0x1301);
        put_u8(hello_body, 0);
        put_u16(hello_body, server_extensions.size());
        put_bytes(hello_body, server_extensions);
        auto hello = bytes{};
        put_u8(hello, 2);
        put_u24(hello, hello_body.size());
        put_bytes(hello, hello_body);
        put_u8(reply_, 22);
        put_u16(reply_, 0x0303);
        put_u16(reply_, hello.size());
        put_bytes(reply_, hello);

        auto transcript = join_bytes(client_hello, hello);
        auto secret = nxt::crypto::x25519_dh(key.secret_key, client_share);
        fixture_check(secret.has_value());
        auto keys = derive_tls13_handshake_keys(*secret, transcript);
        auto flight = bytes{};
        for (auto type : types_) {
            auto message = bytes{};
            auto content = bytes{};
            if (type == 8) {
                put_u16(content, 0); // EncryptedExtensions
            } else if (type == 11) {
                auto entries = bytes{};
                for (auto const & der : fixture_.chain().chain_der) {
                    put_u24(entries, der.size());
                    put_bytes(entries, der);
                    put_u16(entries, 0);
                }
                put_u8(content, 0);
                put_u24(content, entries.size());
                put_bytes(content, entries);
            } else if (type == 15) {
                auto input = certificate_verify_message(transcript);
                auto signer =
                    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>{
                        EVP_MD_CTX_new(), EVP_MD_CTX_free};
                fixture_check(signer != nullptr);
                fixture_check(
                    EVP_DigestSignInit(
                        signer.get(),
                        nullptr,
                        EVP_sha256(),
                        nullptr,
                        fixture_.leaf_key.get())
                    == 1);
                fixture_check(
                    EVP_DigestSignUpdate(
                        signer.get(), input.data(), input.size())
                    == 1);
                std::size_t length = 0;
                fixture_check(
                    EVP_DigestSignFinal(signer.get(), nullptr, &length)
                    == 1);
                auto signature = bytes(length);
                fixture_check(
                    EVP_DigestSignFinal(
                        signer.get(),
                        reinterpret_cast<unsigned char *>(signature.data()),
                        &length)
                    == 1);
                signature.resize(length);
                if (corrupt_signature_)
                    signature.back() ^= std::byte{1};
                put_u16(content, 0x0403);
                put_u16(content, signature.size());
                put_bytes(content, signature);
            } else if (type == 20) {
                content = finished_verify_data(
                    keys.server.traffic_secret, transcript);
            }
            put_u8(message, type);
            put_u24(message, content.size());
            put_bytes(message, content);
            put_bytes(transcript, message);
            put_bytes(flight, message);
        }
        // Split headers and bodies, not just between messages.
        auto remaining = std::span{flight};
        while (!remaining.empty()) {
            auto count =
                remaining.size() <= 64 ? remaining.size() : std::size_t{17};
            put_bytes(
                reply_,
                seal_tls13_record(keys.server, 22, remaining.first(count)));
            remaining = remaining.subspan(count);
        }
        return nxtrt::hope<std::size_t>::ready(sent.size());
    }

    nxt::tls::bytes & reply_;
    certificate_fixture & fixture_;
    std::vector<int> types_;
    bool corrupt_signature_;
};

} // namespace

using namespace boost::ut;
using namespace std::literals;

suite tls_tests = [] {
    "TLS authentication"_group = [] {
        "validates chains and SAN identities, not common names"_test = [] {
            auto fixture = certificate_fixture{};
            auto chain = fixture.chain();
            for (auto host :
                 {"service.example",
                  "SERVICE.EXAMPLE",
                  "one.wild.example",
                  "127.0.0.1",
                  "::1"})
                expect(certificate_failure(chain, host, fixture.path.data())
                           .empty());
            for (auto host :
                 {"wrong.example",
                  "cn.example",
                  "two.one.wild.example",
                  "wild.example",
                  "127.0.0.2",
                  "::2",
                  "",
                  "*.wild.example"})
                expect(
                    !certificate_failure(chain, host, fixture.path.data())
                         .empty());
            expect(
                !certificate_failure(
                     chain, "service.example\0.evil"sv, fixture.path.data())
                     .empty());

            certificate_extension(
                fixture.leaf.get(),
                NID_subject_alt_name,
                "DNS:127.0.0.1,DNS:f*.partial.example");
            fixture.sign_leaf();
            expect(!certificate_failure(
                        fixture.chain(), "127.0.0.1", fixture.path.data())
                        .empty());
            expect(!certificate_failure(
                        fixture.chain(),
                        "foo.partial.example",
                        fixture.path.data())
                        .empty());
            X509_EXTENSION_free(X509_delete_ext(
                fixture.leaf.get(),
                X509_get_ext_by_NID(
                    fixture.leaf.get(), NID_subject_alt_name, -1)));
            fixture.sign_leaf();
            expect(!certificate_failure(
                        fixture.chain(), "cn.example", fixture.path.data())
                        .empty());
        };

        "rejects untrusted, incomplete, forged and malformed chains"_test =
            [] {
                auto fixture = certificate_fixture{};
                auto other = certificate_fixture{};
                auto chain = fixture.chain();
                expect(!certificate_failure(
                            chain, "service.example", other.path.data())
                            .empty());
                // Even a root supplied by the peer must not become trusted.
                chain.chain_der.push_back(
                    certificate_der(fixture.root.get()));
                expect(!certificate_failure(
                            chain, "service.example", other.path.data())
                            .empty());
                chain = fixture.chain();
                chain.chain_der.pop_back();
                expect(!certificate_failure(
                            chain, "service.example", fixture.path.data())
                            .empty());
                chain = fixture.chain();
                chain.chain_der[0].back() ^= std::byte{1};
                expect(!certificate_failure(
                            chain, "service.example", fixture.path.data())
                            .empty());
                chain = fixture.chain();
                chain.chain_der[0].push_back(std::byte{0});
                expect(
                    certificate_failure(
                        chain, "service.example", fixture.path.data())
                    == "invalid DER certificate");
                expect(!certificate_failure(
                            fixture.chain(),
                            "service.example",
                            "/nonexistent/nxt-ca.pem")
                            .empty());
            };

        "enforces validity, server purpose, signing usage and CA constraints"_test =
            [] {
                auto fixture = certificate_fixture{};
                fixture_check(
                    X509_gmtime_adj(
                        X509_getm_notAfter(fixture.leaf.get()), -1)
                    != nullptr);
                fixture.sign_leaf();
                expect(
                    certificate_failure(
                        fixture.chain(),
                        "service.example",
                        fixture.path.data())
                        .find("expired")
                    != std::string::npos);
                fixture_check(
                    X509_gmtime_adj(
                        X509_getm_notAfter(fixture.leaf.get()), 3600)
                    != nullptr);
                fixture_check(
                    X509_gmtime_adj(
                        X509_getm_notBefore(fixture.leaf.get()), 600)
                    != nullptr);
                fixture.sign_leaf();
                expect(
                    certificate_failure(
                        fixture.chain(),
                        "service.example",
                        fixture.path.data())
                        .find("not yet valid")
                    != std::string::npos);
                fixture_check(
                    X509_gmtime_adj(
                        X509_getm_notBefore(fixture.leaf.get()), -60)
                    != nullptr);
                certificate_extension(
                    fixture.leaf.get(), NID_ext_key_usage, "clientAuth");
                fixture.sign_leaf();
                expect(!certificate_failure(
                            fixture.chain(),
                            "service.example",
                            fixture.path.data())
                            .empty());
                certificate_extension(
                    fixture.leaf.get(), NID_ext_key_usage, "serverAuth");
                certificate_extension(
                    fixture.leaf.get(), NID_key_usage, "keyEncipherment");
                fixture.sign_leaf();
                expect(
                    certificate_failure(
                        fixture.chain(),
                        "service.example",
                        fixture.path.data())
                        .find("digital signatures")
                    != std::string::npos);
                certificate_extension(
                    fixture.leaf.get(), NID_key_usage, "digitalSignature");
                fixture.sign_leaf();
                certificate_extension(
                    fixture.intermediate.get(),
                    NID_basic_constraints,
                    "critical,CA:FALSE");
                fixture_check(
                    X509_sign(
                        fixture.intermediate.get(),
                        fixture.root_key.get(),
                        EVP_sha256())
                    > 0);
                expect(!certificate_failure(
                            fixture.chain(),
                            "service.example",
                            fixture.path.data())
                            .empty());
            };

        "rejects unknown critical extensions and expired intermediates"_test =
            [] {
                auto fixture = certificate_fixture{};
                auto oid = std::
                    unique_ptr<ASN1_OBJECT, decltype(&ASN1_OBJECT_free)>{
                        OBJ_txt2obj("1.2.3.4.5.6.7", 1), ASN1_OBJECT_free};
                auto value = std::unique_ptr<
                    ASN1_OCTET_STRING,
                    decltype(&ASN1_OCTET_STRING_free)>{
                    ASN1_OCTET_STRING_new(), ASN1_OCTET_STRING_free};
                fixture_check(oid != nullptr && value != nullptr);
                const unsigned char encoded[] = {5, 0}; // DER NULL
                fixture_check(
                    ASN1_OCTET_STRING_set(value.get(), encoded, 2) == 1);
                auto extension = std::unique_ptr<
                    X509_EXTENSION,
                    decltype(&X509_EXTENSION_free)>{
                    X509_EXTENSION_create_by_OBJ(
                        nullptr, oid.get(), 1, value.get()),
                    X509_EXTENSION_free};
                fixture_check(extension != nullptr);
                fixture_check(
                    X509_add_ext(fixture.leaf.get(), extension.get(), -1)
                    == 1);
                fixture.sign_leaf();
                expect(
                    certificate_failure(
                        fixture.chain(),
                        "service.example",
                        fixture.path.data())
                        .find("critical extension")
                    != std::string::npos);
                X509_EXTENSION_free(X509_delete_ext(
                    fixture.leaf.get(),
                    X509_get_ext_count(fixture.leaf.get()) - 1));
                fixture.sign_leaf();
                fixture_check(
                    X509_gmtime_adj(
                        X509_getm_notAfter(fixture.intermediate.get()), -1)
                    != nullptr);
                fixture_check(
                    X509_sign(
                        fixture.intermediate.get(),
                        fixture.root_key.get(),
                        EVP_sha256())
                    > 0);
                expect(
                    certificate_failure(
                        fixture.chain(),
                        "service.example",
                        fixture.path.data())
                        .find("expired")
                    != std::string::npos);
            };

        "requires ordered certificate authentication before Finished"_test =
            [] {
                auto fixture = certificate_fixture{};
                for (auto const & types : std::vector<std::vector<int>>{
                         {8, 11, 15, 20},
                         {8, 20},
                         {8, 11, 20},
                         {11, 8, 15, 20},
                         {8, 11, 11, 15, 20},
                         {8, 11, 15, 20, 8}}) {
                    auto chunks = std::array<nxt::tls::bytes, 1>{};
                    auto reader =
                        nxtrt::byte_span_feed{chunks, std::size_t{65536}};
                    auto peer = handshake_peer{chunks[0], fixture, types};
                    auto client =
                        nxtrt::tls::tls13_client_session{reader, peer};
                    auto deck = nxtrt::deck{};
                    auto failure = std::string{};
                    try {
                        deck.sync_wait([&] {
                            return client.handshake(
                                "service.example", fixture.path.data());
                        });
                    } catch (nxtrt::runtime_error const & error) {
                        failure = error.what();
                    }
                    auto valid = types == std::vector<int>{8, 11, 15, 20};
                    expect(failure.empty() == valid);
                    if (!valid)
                        expect(
                            failure.find("unexpected")
                            != std::string::npos);
                    // No client Finished is sent when authentication fails.
                    expect(peer.writes == (valid ? 2 : 1));
                }
            };

        "rejects forged CertificateVerify even with a trusted chain"_test =
            [] {
                auto fixture = certificate_fixture{};
                auto chunks = std::array<nxt::tls::bytes, 1>{};
                auto reader =
                    nxtrt::byte_span_feed{chunks, std::size_t{65536}};
                auto peer = handshake_peer{
                    chunks[0], fixture, {8, 11, 15, 20}, true};
                auto client =
                    nxtrt::tls::tls13_client_session{reader, peer};
                auto deck = nxtrt::deck{};
                auto failure = std::string{};
                try {
                    deck.sync_wait([&] {
                        return client.handshake(
                            "service.example", fixture.path.data());
                    });
                } catch (nxtrt::runtime_error const & error) {
                    failure = error.what();
                }
                expect(failure == "CertificateVerify signature failed");
                expect(peer.writes == 1);
            };
    };
};

} // namespace nxt::test
