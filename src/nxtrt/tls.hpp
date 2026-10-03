#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/net.hpp"
#include "nxtrt/task.hpp"
#include "nxt/tls.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/**
 * @namespace nxtrt::tls
 * An experimental TLS 1.3 client over runtime byte streams.
 *
 * `tls13_client_session` runs the handshake over any borrowed
 * `bytefeed`/`bytesink` pair (usually a TCP socket's) and is then itself a
 * `bytefeed` of decrypted application data. It authenticates the server
 * with libcrypto's X.509 verifier and a strict host-name match; there is no
 * option to disable verification. It is handmade and not audited: no
 * OCSP/CRL revocation checks, no Certificate Transparency, no session
 * resumption, no client certificates. The record and key-schedule code is
 * in `nxt::tls`.
 */
namespace nxtrt::tls {

/// Record content type, as `tls_event_kind_from_content_type` maps it.
enum class tls_event_kind
{
    change_cipher_spec,
    handshake,
    application_data,
    alert,
    unknown,
};

/// One record received after the handshake (see
/// `tls13_client_session::next_event`).
struct tls13_session_event
{
    tls_event_kind kind = tls_event_kind::unknown;
    std::uint8_t content_type = 0;
    /// Borrows the session's record storage until the following
    /// `next_event()` call.
    std::span<std::byte> content;
};

tls_event_kind tls_event_kind_from_content_type(std::uint8_t type);

/// A TLS 1.3 client connection layered on a borrowed byte reader and
/// writer.
///
/// Construct it over the transport, `co_await handshake(host)`, then write
/// with `write_all` and read decrypted application data through the
/// `bytefeed` interface (or record by record with `next_event`). Every
/// operation except `handshake` throws `nxtrt::runtime_error` until the
/// handshake has completed.
///
/// Protocol scope: TLS 1.3 only, cipher suite TLS_AES_128_GCM_SHA256 only,
/// X25519 key exchange only, server signatures ecdsa_secp256r1_sha256 or
/// rsa_pss_rsae_sha256, ALPN "http/1.1". No PSK, 0-RTT or client
/// authentication. Post-handshake handshake messages (such as
/// NewSessionTicket) are skipped by bytefeed reads. KeyUpdate rotates
/// inbound traffic keys and, when requested, responds and rotates outbound
/// keys before further application writes.
///
/// Ownership: the reader and writer (or the `net::socket`) are borrowed and
/// must outlive the session. The session is deck-confined, like the
/// streams under it.
///
/// Ending: close_notify reports bytefeed EOF (and `read()` throws
/// `end_of_stream`); other alerts throw `nxtrt::runtime_error`. End of the
/// transport without close_notify, even between records, throws
/// `end_of_stream`. Callers reading responses should rely on HTTP framing
/// (Content-Length or chunked), not on connection close.
class tls13_client_session final : public bytefeed
{
public:
    tls13_client_session(
        bytefeed & reader,
        bytesink & writer,
        std::size_t buffer_size = 4096);

    tls13_client_session(
        bytefeed & reader,
        bytesink & writer,
        std::span<std::byte> buffer);

    tls13_client_session(
        net::socket & socket,
        std::span<std::byte> buffer);

    tls13_client_session(
        net::socket & socket,
        std::size_t buffer_size = 4096);

    /// Runs the full handshake and authenticates the server as HOST.
    ///
    /// HOST is a DNS name or an unbracketed IPv4/IPv6 literal. A DNS name
    /// is sent as SNI and must match a DNS subject alternative name (only
    /// whole-label wildcards; no common-name fallback). An IP literal sends
    /// no SNI and must match an IP SAN. The chain is verified by
    /// libcrypto's X.509 verifier (trust, signatures, validity dates, CA
    /// constraints, critical extensions, TLS server purpose, depth at most
    /// 10); the server's intermediates are used only as untrusted chain
    /// material. The leaf must allow digital signatures if it has a key
    /// usage extension. CertificateVerify and the server Finished are
    /// checked before the client Finished is sent.
    ///
    /// Trust: with CA_FILE empty, libcrypto's default paths, which honor
    /// `SSL_CERT_FILE` and `SSL_CERT_DIR`. Otherwise CA_FILE names a PEM
    /// bundle that replaces the default store. There is no way to skip
    /// verification. The server's handshake flight is bounded to 1 MiB.
    ///
    /// Throws `nxtrt::runtime_error` (with the verifier's reason, where
    /// there is one) on any protocol or verification failure; the session
    /// must not be used afterwards.
    task<> handshake(std::string_view host, std::string_view ca_file = {});
    /// Encrypts BYTES as application data records of at most 16 KiB,
    /// writes them to the writer and flushes it.
    task<> write_all(std::span<const std::byte> bytes);
    task<> write_all(std::string_view text);
    /// The next non-alert record as an owned copy. close_notify throws
    /// `end_of_stream`; other alerts throw `runtime_error`.
    task<nxt::tls::tls13_plaintext> read();
    /// The next record, decrypted unless it is change_cipher_spec. CONTENT
    /// borrows the session's record storage until the next call. KeyUpdate
    /// is handled internally; close_notify is returned as an alert event.
    /// Other alerts throw `runtime_error`.
    task<tls13_session_event> next_event();

private:
    static std::uint16_t parse_record_u16(
        std::span<const std::byte, 2> bytes) noexcept;

    task<> read_exact(std::span<std::byte> dst);
    task<> read_record_into_storage();
    [[nodiscard]] std::span<std::byte> record_payload() noexcept;

    hope<fare_t> stream_more(
        bytesink & writer,
        std::size_t limit) override;

    task<fare_t> stream_more_task(
        bytesink & writer,
        std::size_t limit);

    hope<fare_t> copy_pending(
        bytesink & writer,
        std::size_t limit);

    task<fare_t> copy_pending_slow(
        hope<void> write,
        std::size_t n);

    void require_handshake() const;

    bytefeed & reader_;
    bytesink & writer_;
    nxt::tls::tls13_application_keys application_keys_;
    std::vector<std::byte> record_storage_;
    std::vector<std::byte> post_handshake_;
    std::vector<std::byte> pending_;
    std::size_t pending_offset_ = 0;
    std::uint8_t record_type_ = 0;
    std::uint16_t record_version_ = 0;
    bool handshaken_ = false;
    bool close_notify_received_ = false;
};

} // namespace nxtrt::tls
