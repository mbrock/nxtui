#include "nxtrt/tls.hpp"

#include "nxt/tls/cert.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace nxtrt::tls {

tls_event_kind tls_event_kind_from_content_type(std::uint8_t type)
{
    switch (type) {
    case 20:
        return tls_event_kind::change_cipher_spec;
    case 21:
        return tls_event_kind::alert;
    case 22:
        return tls_event_kind::handshake;
    case 23:
        return tls_event_kind::application_data;
    default:
        return tls_event_kind::unknown;
    }
}

tls13_client_session::tls13_client_session(
    bytefeed & reader,
    bytesink & writer,
    std::size_t buffer_size)
    : bytefeed(buffer_size)
    , reader_(reader)
    , writer_(writer)
{}

tls13_client_session::tls13_client_session(
    bytefeed & reader,
    bytesink & writer,
    std::span<std::byte> buffer)
    : bytefeed(buffer)
    , reader_(reader)
    , writer_(writer)
{}

tls13_client_session::tls13_client_session(
    net::socket & socket,
    std::span<std::byte> buffer)
    : tls13_client_session(socket.input(), socket.output(), buffer)
{}

tls13_client_session::tls13_client_session(
    net::socket & socket,
    std::size_t buffer_size)
    : tls13_client_session(socket.input(), socket.output(), buffer_size)
{}

task<> tls13_client_session::handshake(
    std::string_view host, std::string_view ca_file)
{
    handshaken_ = false;
    auto hello = nxt::tls::make_tls13_client_hello(host);
    co_await nxtrt::write_all(writer_, hello.record);

    auto record = co_await nxt::tls::read_tls_record(reader_);
    auto server_hello = nxt::tls::parse_tls13_server_hello(record);
    nxt::tls::require_tls(
        server_hello.legacy_session_id == hello.legacy_session_id,
        "server did not echo the legacy session ID");
    auto shared_secret = nxt::crypto::x25519_dh(
        hello.key_pair.secret_key, server_hello.key_share);
    nxt::tls::require_tls(
        shared_secret.has_value(), "X25519 shared secret failed");

    auto transcript =
        nxt::tls::join_bytes(hello.handshake, server_hello.handshake);
    auto handshake_keys =
        nxt::tls::derive_tls13_handshake_keys(*shared_secret, transcript);

    auto leaf_public_key = std::optional<nxt::tls::leaf_public_key>{};
    // This client offers neither PSK nor client authentication. Require the
    // full server-authentication flight, in order, before accepting
    // Finished.
    constexpr auto expected_types = std::array{8, 11, 15, 20};
    auto next_message = std::size_t{0};
    auto flight = nxt::tls::bytes{};
    while (next_message < expected_types.size()) {
        co_await read_record_into_storage();
        if (record_type_ == 20) {
            nxt::tls::require_tls(
                record_version_ == 0x0303 && record_payload().size() == 1
                    && record_payload()[0] == std::byte{1},
                "invalid change_cipher_spec");
            continue;
        }

        auto plaintext = nxt::tls::open_tls13_record_in_place(
            handshake_keys.server,
            record_type_,
            record_version_,
            record_payload());
        nxt::tls::require_tls(
            plaintext.inner_type == 22,
            "expected server handshake, received alert or application data");

        // Certificate chains routinely span records. Only process complete
        // messages, with a bound on retained handshake data.
        constexpr auto max_flight_size = std::size_t{1024 * 1024};
        nxt::tls::require_tls(
            plaintext.content.size() <= max_flight_size - flight.size(),
            "TLS handshake is too large");
        nxt::tls::put_bytes(flight, plaintext.content);
        auto consumed = std::size_t{0};
        while (flight.size() - consumed >= 4) {
            auto remaining = std::span{flight}.subspan(consumed);
            auto size = std::size_t{4}
                        + nxt::tls::parse_u24(remaining.subspan(1, 3));
            nxt::tls::require_tls(
                size <= max_flight_size, "TLS handshake is too large");
            if (remaining.size() < size)
                break;
            auto message = remaining.first(size);
            auto type = std::to_integer<std::uint8_t>(message[0]);
            nxt::tls::require_tls(
                next_message < expected_types.size()
                    && type == expected_types[next_message],
                "unexpected server handshake message or missing authentication");
            if (type == 11) {
                auto cert = nxt::tls::parse_tls13_certificate(message);
                nxt::tls::verify_server_certificate(cert, host, ca_file);
                leaf_public_key =
                    nxt::tls::extract_leaf_public_key(cert.leaf_der);
            } else if (type == 15) {
                auto cert_verify =
                    nxt::tls::parse_tls13_certificate_verify(message);
                nxt::tls::require_tls(
                    cert_verify.scheme == 0x0403
                        || cert_verify.scheme == 0x0804,
                    "server used an unoffered CertificateVerify scheme");
                auto ok = nxt::tls::verify_certificate_verify(
                    *leaf_public_key, transcript, cert_verify);
                nxt::tls::require_tls(
                    ok, "CertificateVerify signature failed");
            } else if (type == 20) {
                auto received = nxt::tls::parse_tls13_finished(message);
                auto ok = nxt::tls::verify_finished(
                    handshake_keys.server.traffic_secret,
                    transcript,
                    received);
                nxt::tls::require_tls(ok, "server Finished failed");
            }
            nxt::tls::put_bytes(transcript, message);
            consumed += size;
            ++next_message;
        }
        flight.erase(flight.begin(), flight.begin() + consumed);
        nxt::tls::require_tls(
            next_message < expected_types.size() || flight.empty(),
            "unexpected handshake bytes after server Finished");
    }

    application_keys_ = nxt::tls::derive_tls13_application_keys(
        handshake_keys.secret, transcript);
    auto client_finished = nxt::tls::make_finished_message(
        handshake_keys.client.traffic_secret, transcript);
    auto finished_record = nxt::tls::seal_tls13_record(
        handshake_keys.client, 22, client_finished);
    co_await nxtrt::write_all(writer_, finished_record);

    nxt::tls::put_bytes(transcript, client_finished);
    handshaken_ = true;
}

task<> tls13_client_session::write_all(std::span<const std::byte> bytes)
{
    require_handshake();
    // RFC 8446 section 5.1 limits each plaintext fragment to 2^14 bytes.
    // Tool continuations routinely exceed that, even with short answers.
    while (!bytes.empty()) {
        auto count = std::min(bytes.size(), std::size_t{16384});
        co_await nxtrt::write_all(
            writer_,
            nxt::tls::seal_tls13_record(
                application_keys_.client, 23, bytes.first(count)));
        bytes = bytes.subspan(count);
    }
}

task<> tls13_client_session::write_all(std::string_view text)
{
    co_await write_all(nxtrt::as_bytes(text));
}

task<nxt::tls::tls13_plaintext> tls13_client_session::read()
{
    while (true) {
        auto event = co_await next_event();
        if (event.kind == tls_event_kind::alert)
            throw runtime_error{"received TLS alert"};
        co_return nxt::tls::tls13_plaintext{
            .content = nxt::tls::bytes{
                event.content.begin(),
                event.content.end(),
            },
            .inner_type = event.content_type,
        };
    }
}

task<tls13_session_event> tls13_client_session::next_event()
{
    require_handshake();

    co_await read_record_into_storage();
    if (record_type_ == 20) {
        co_return tls13_session_event{
            .kind = tls_event_kind::change_cipher_spec,
            .content_type = record_type_,
            .content = record_payload(),
        };
    }

    auto plaintext = nxt::tls::open_tls13_record_in_place(
        application_keys_.server,
        record_type_,
        record_version_,
        record_payload());
    co_return tls13_session_event{
        .kind = tls_event_kind_from_content_type(plaintext.inner_type),
        .content_type = plaintext.inner_type,
        .content = plaintext.content,
    };
}

std::uint16_t tls13_client_session::parse_record_u16(
    std::span<const std::byte, 2> bytes) noexcept
{
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(bytes[0]) << 8)
        | std::to_integer<std::uint16_t>(bytes[1]));
}

task<> tls13_client_session::read_exact(std::span<std::byte> dst)
{
    while (!dst.empty()) {
        auto n = co_await reader_.read(dst);
        if (is_eof(n))
            throw end_of_stream{"unexpected end of TLS input"};
        if (value_count(n) == 0)
            continue;
        dst = dst.subspan(value_count(n));
    }
}

task<> tls13_client_session::read_record_into_storage()
{
    auto header = std::array<std::byte, 5>{};
    co_await read_exact(header);

    record_type_ = std::to_integer<std::uint8_t>(header[0]);
    record_version_ = parse_record_u16(
        std::span<const std::byte, 2>{header.data() + 1, 2});
    auto length = parse_record_u16(
        std::span<const std::byte, 2>{header.data() + 3, 2});

    record_storage_.resize(length);
    co_await read_exact(record_storage_);
}

std::span<std::byte> tls13_client_session::record_payload() noexcept
{
    return record_storage_;
}

hope<fare_t> tls13_client_session::stream_more(
    bytesink & writer,
    std::size_t limit)
{
    require_handshake();
    if (limit == 0)
        return hope<fare_t>::ready(0);

    if (pending_offset_ < pending_.size())
        return copy_pending(writer, limit);

    return stream_more_task(writer, limit);
}

task<fare_t> tls13_client_session::stream_more_task(
    bytesink & writer,
    std::size_t limit)
{
    pending_.clear();
    pending_offset_ = 0;
    do {
        auto event = co_await next_event();
        if (event.kind == tls_event_kind::alert)
            throw runtime_error{"received TLS alert"};
        if (event.kind == tls_event_kind::application_data) {
            pending_.assign(event.content.begin(), event.content.end());
            break;
        }
    } while (pending_.empty());

    co_return co_await copy_pending(writer, limit);
}

hope<fare_t> tls13_client_session::copy_pending(
    bytesink & writer,
    std::size_t limit)
{
    auto pending = std::span{pending_}.subspan(pending_offset_);
    auto n = std::min(limit, pending.size());
    auto dst = writer.unused_capacity();
    if (!dst.empty())
        n = std::min(n, dst.size());

    auto write = writer.write(pending.first(n));
    if (write.is_ready()) {
        pending_offset_ += n;
        return hope<fare_t>::ready(n);
    }
    return copy_pending_slow(std::move(write), n);
}

task<fare_t> tls13_client_session::copy_pending_slow(
    hope<void> write,
    std::size_t n)
{
    co_await std::move(write);
    pending_offset_ += n;
    co_return n;
}

void tls13_client_session::require_handshake() const
{
    if (!handshaken_)
        throw runtime_error{"TLS session has not completed handshake"};
}

} // namespace nxtrt::tls
