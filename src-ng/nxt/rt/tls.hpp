#pragma once

#include "nxt/rt/buffers.hpp"
#include "nxt/rt/task.hpp"
#include "nxt/tls.hpp"
#include "nxt/tls/cert.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::rt::tls {

template<typename Reader, typename Writer>
class tls13_client_session
{
public:
    tls13_client_session(Reader & reader, Writer & writer)
        : reader_(reader)
        , writer_(writer)
    {
    }

    task<> handshake(std::string_view host)
    {
        auto hello = nxt::tls::make_tls13_client_hello(host);
        co_await writer_.write_all(hello.record);

        auto record = co_await nxt::tls::read_tls_record(reader_);
        auto server_hello = nxt::tls::parse_tls13_server_hello(record);
        auto shared_secret = nxt::crypto::x25519_dh(
            hello.key_pair.secret_key, server_hello.key_share);
        nxt::tls::require_tls(
            shared_secret.has_value(), "X25519 shared secret failed");

        auto transcript =
            nxt::tls::join_bytes(hello.handshake, server_hello.handshake);
        auto handshake_keys =
            nxt::tls::derive_tls13_handshake_keys(*shared_secret, transcript);

        auto leaf_public_key = std::optional<nxt::tls::bytes>{};
        auto saw_server_finished = false;
        while (!saw_server_finished) {
            record = co_await nxt::tls::read_tls_record(reader_);
            if (record.type == 20)
                continue;

            auto plaintext =
                nxt::tls::open_tls13_record(handshake_keys.server, record);
            if (plaintext.inner_type != 22)
                continue;

            for (auto const & message :
                 nxt::tls::split_handshake_messages(plaintext.content)) {
                auto type = std::to_integer<std::uint8_t>(message[0]);
                if (type == 11) {
                    auto cert = nxt::tls::parse_tls13_certificate(message);
                    leaf_public_key =
                        nxt::tls::extract_p256_public_key_from_certificate(
                            cert.leaf_der);
                } else if (type == 15) {
                    nxt::tls::require_tls(
                        leaf_public_key.has_value(),
                        "certificate_verify arrived before certificate");
                    auto cert_verify =
                        nxt::tls::parse_tls13_certificate_verify(message);
                    auto ok = nxt::tls::verify_certificate_verify(
                        *leaf_public_key, transcript, cert_verify);
                    nxt::tls::require_tls(
                        ok, "CertificateVerify signature failed");
                } else if (type == 20) {
                    auto received =
                        nxt::tls::parse_tls13_finished(message);
                    auto ok = nxt::tls::verify_finished(
                        handshake_keys.server.traffic_secret,
                        transcript,
                        received);
                    nxt::tls::require_tls(ok, "server Finished failed");
                    saw_server_finished = true;
                }
                nxt::tls::put_bytes(transcript, message);
            }
        }

        application_keys_ = nxt::tls::derive_tls13_application_keys(
            handshake_keys.secret, transcript);
        auto client_finished = nxt::tls::make_finished_message(
            handshake_keys.client.traffic_secret, transcript);
        co_await writer_.write_all(
            nxt::tls::seal_tls13_record(
                handshake_keys.client, 22, client_finished));
        nxt::tls::put_bytes(transcript, client_finished);
        handshaken_ = true;
    }

    task<> write_all(std::span<const std::byte> bytes)
    {
        require_handshake();
        co_await writer_.write_all(
            nxt::tls::seal_tls13_record(
                application_keys_.client, 23, bytes));
    }

    task<> write_all(std::string_view text)
    {
        co_await write_all(nxt::rt::as_bytes(text));
    }

    task<nxt::tls::tls13_plaintext> read()
    {
        require_handshake();
        while (true) {
            auto record = co_await nxt::tls::read_tls_record(reader_);
            auto plaintext =
                nxt::tls::open_tls13_record(application_keys_.server, record);
            if (plaintext.inner_type == 21)
                throw runtime_error{"received TLS alert"};
            co_return plaintext;
        }
    }

    task<read_result> read_some(std::span<std::byte> dst)
    {
        require_handshake();
        if (dst.empty())
            co_return read_result{.bytes = 0, .eof = false};

        if (pending_offset_ == pending_.size()) {
            pending_.clear();
            pending_offset_ = 0;
            do {
                auto plaintext = co_await read();
                if (plaintext.inner_type == 23) {
                    pending_ = std::move(plaintext.content);
                    break;
                }
            } while (pending_.empty());
        }

        auto pending = std::span{pending_}.subspan(pending_offset_);
        auto n = std::min(dst.size(), pending.size());
        std::memcpy(dst.data(), pending.data(), n);
        pending_offset_ += n;
        co_return read_result{.bytes = n, .eof = false};
    }

private:
    void require_handshake() const
    {
        if (!handshaken_)
            throw runtime_error{"TLS session has not completed handshake"};
    }

    Reader & reader_;
    Writer & writer_;
    nxt::tls::tls13_application_keys application_keys_;
    std::vector<std::byte> pending_;
    std::size_t pending_offset_ = 0;
    bool handshaken_ = false;
};

template<typename Reader, typename Writer>
tls13_client_session(Reader &, Writer &) -> tls13_client_session<Reader, Writer>;

} // namespace nxt::rt::tls
