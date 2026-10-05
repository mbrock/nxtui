#include "nxt/tls/cert.hpp"

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <fstream>
#else
#  include <arpa/inet.h>
#endif
#include <memory>

namespace nxt::tls {

bool is_ip_address(std::string_view host)
{
    auto text = std::string{host};
    auto address = std::array<unsigned char, 16>{};
    return inet_pton(AF_INET, text.c_str(), address.data()) == 1
           || inet_pton(AF_INET6, text.c_str(), address.data()) == 1;
}

void verify_server_certificate(
    tls13_certificate const & certificate,
    std::string_view host,
    std::string_view ca_file)
{
    validate_tls_host(host);
    require_tls(
        !certificate.chain_der.empty(), "certificate list is empty");
    require_tls(
        ca_file.find('\0') == std::string_view::npos,
        "invalid TLS CA file path");

    auto store = std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)>{
        X509_STORE_new(), X509_STORE_free};
    require_tls(store != nullptr, "cannot allocate TLS trust store");
    if (ca_file.empty()) {
#if defined(_WIN32)
        // UWP has no Unix trust paths. Require provisioned app-local roots;
        // never fall back to an empty or unverified store.
        throw nxtrt::runtime_error{
            "Windows TLS requires an explicit PEM CA bundle"};
#else
        // libcrypto honors SSL_CERT_FILE and SSL_CERT_DIR here.
        require_tls(
            X509_STORE_set_default_paths(store.get()) == 1,
            "cannot load default TLS trust store");
#endif
    } else {
        auto path = std::string{ca_file};
#if defined(_WIN32)
        // UWP libcrypto is built without stdio. Application CRT file access
        // loads app-local public roots into a memory BIO instead.
        auto file = std::ifstream{path, std::ios::binary};
        require_tls(bool(file), "cannot open TLS CA file");
        auto pem = std::string(1024 * 1024 + 1, '\0');
        file.read(pem.data(), static_cast<std::streamsize>(pem.size()));
        require_tls(
            file.eof() && !file.bad() && file.gcount() > 0
                && file.gcount() <= 1024 * 1024,
            "invalid TLS CA bundle size or read failure");
        pem.resize(static_cast<std::size_t>(file.gcount()));
        auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>{
            BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
            BIO_free};
        require_tls(bio != nullptr, "cannot allocate TLS CA reader");
        auto free_roots = [](STACK_OF(X509_INFO) * roots) {
            sk_X509_INFO_pop_free(roots, X509_INFO_free);
        };
        auto roots =
            std::unique_ptr<STACK_OF(X509_INFO), decltype(free_roots)>{
                PEM_X509_INFO_read_bio(
                    bio.get(), nullptr, nullptr, nullptr),
                free_roots};
        require_tls(roots != nullptr, "cannot parse TLS CA bundle");
        int count = 0;
        for (int i = 0; i < sk_X509_INFO_num(roots.get()); ++i) {
            auto * info = sk_X509_INFO_value(roots.get(), i);
            if (info->x509) {
                require_tls(
                    X509_STORE_add_cert(store.get(), info->x509) == 1,
                    "cannot add TLS trust root");
                ++count;
            }
        }
        require_tls(count != 0, "TLS CA bundle has no certificates");
#else
        require_tls(
            X509_STORE_load_locations(store.get(), path.c_str(), nullptr)
                == 1,
            "cannot load TLS CA file");
#endif
    }

    // Peer's intermediates are untrusted, never added to the trust store.
    auto free_chain = [](STACK_OF(X509) * chain) {
        sk_X509_pop_free(chain, X509_free);
    };
    auto chain = std::unique_ptr<STACK_OF(X509), decltype(free_chain)>{
        sk_X509_new_null(), free_chain};
    require_tls(chain != nullptr, "cannot allocate TLS certificate chain");
    for (auto const & der : certificate.chain_der) {
        auto begin = reinterpret_cast<const unsigned char *>(der.data());
        auto cursor = begin;
        auto cert = std::unique_ptr<X509, decltype(&X509_free)>{
            d2i_X509(nullptr, &cursor, static_cast<long>(der.size())),
            X509_free};
        require_tls(
            cert != nullptr && cursor == begin + der.size(),
            "invalid DER certificate");
        require_tls(
            sk_X509_push(chain.get(), cert.get()) != 0,
            "cannot append TLS certificate");
        cert.release();
    }

    auto leaf = sk_X509_value(chain.get(), 0);
    // TLS 1.3 authenticates with a signature, not RSA key encipherment.
    auto usage =
        std::unique_ptr<ASN1_BIT_STRING, decltype(&ASN1_BIT_STRING_free)>{
            static_cast<ASN1_BIT_STRING *>(
                X509_get_ext_d2i(leaf, NID_key_usage, nullptr, nullptr)),
            ASN1_BIT_STRING_free};
    require_tls(
        X509_get_ext_by_NID(leaf, NID_key_usage, -1) < 0
            || (usage && ASN1_BIT_STRING_get_bit(usage.get(), 0) == 1),
        "TLS certificate does not allow digital signatures");

    auto context =
        std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)>{
            X509_STORE_CTX_new(), X509_STORE_CTX_free};
    require_tls(context != nullptr, "cannot allocate TLS verifier");
    require_tls(
        X509_STORE_CTX_init(context.get(), store.get(), leaf, chain.get())
            == 1,
        "cannot initialize TLS verifier");
    require_tls(
        X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_SERVER)
            == 1,
        "cannot configure TLS server certificate purpose");
    auto parameters = X509_STORE_CTX_get0_param(context.get());
    X509_VERIFY_PARAM_set_depth(parameters, 10);
    X509_VERIFY_PARAM_set_hostflags(
        parameters,
        X509_CHECK_FLAG_NEVER_CHECK_SUBJECT
            | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (is_ip_address(host)) {
        auto text = std::string{host};
        require_tls(
            X509_VERIFY_PARAM_set1_ip_asc(parameters, text.c_str()) == 1,
            "cannot configure TLS IP identity");
    } else {
        require_tls(
            X509_VERIFY_PARAM_set1_host(
                parameters, host.data(), host.size())
                == 1,
            "cannot configure TLS DNS identity");
    }
    if (X509_verify_cert(context.get()) != 1) {
        throw nxtrt::runtime_error{
            std::string{"TLS certificate verification failed: "}
            + X509_verify_cert_error_string(
                X509_STORE_CTX_get_error(context.get()))};
    }
}

} // namespace nxt::tls
