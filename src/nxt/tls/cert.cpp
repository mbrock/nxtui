#include "nxt/tls/cert.hpp"

#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
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
        // libcrypto honors SSL_CERT_FILE and SSL_CERT_DIR here.
        require_tls(
            X509_STORE_set_default_paths(store.get()) == 1,
            "cannot load default TLS trust store");
    } else {
        auto path = std::string{ca_file};
        require_tls(
            X509_STORE_load_locations(store.get(), path.c_str(), nullptr)
                == 1,
            "cannot load TLS CA file");
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
