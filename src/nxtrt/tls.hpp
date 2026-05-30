#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/net.hpp"
#include "nxtrt/task.hpp"
#include "nxt/tls.hpp"
#include "nxt/tls/cert.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxtrt::tls {

enum class handshake_progress_kind
{
    begin,
    end,
    event,
};

struct handshake_progress
{
    handshake_progress_kind kind = handshake_progress_kind::event;
    std::string_view name;
};

template<typename Notify, task_factory Body>
task<task_result_t<std::invoke_result_t<Body &>>>
with_handshake_progress_span(
    Notify & notify,
    std::string_view name,
    Body body)
{
    co_await notify(
        handshake_progress{
            .kind = handshake_progress_kind::begin,
            .name = name,
        });
    if constexpr (std::is_void_v<task_result_t<std::invoke_result_t<Body &>>>) {
        co_await std::invoke(body);
        co_await notify(
            handshake_progress{
                .kind = handshake_progress_kind::end,
                .name = name,
            });
    } else {
        auto result = co_await std::invoke(body);
        co_await notify(
            handshake_progress{
                .kind = handshake_progress_kind::end,
                .name = name,
            });
        co_return result;
    }
}

template<typename T>
task<std::decay_t<T>> ready_task(T value)
{
    co_return std::move(value);
}

class tls13_client_session final : public bytefeed
{
public:
    tls13_client_session(
        bytefeed & reader,
        bytesink & writer,
        std::size_t buffer_size = 4096)
        : bytefeed(buffer_size)
        , reader_(reader)
        , writer_(writer)
    {
    }

    tls13_client_session(
        bytefeed & reader,
        bytesink & writer,
        std::span<std::byte> buffer)
        : bytefeed(buffer)
        , reader_(reader)
        , writer_(writer)
    {
    }

    tls13_client_session(
        net::socket & socket,
        std::span<std::byte> buffer)
        : tls13_client_session(socket.input(), socket.output(), buffer)
    {
    }

    tls13_client_session(
        net::socket & socket,
        std::size_t buffer_size = 4096)
        : tls13_client_session(socket.input(), socket.output(), buffer_size)
    {
    }

    task<> handshake(std::string_view host)
    {
        co_await handshake(host, [](const handshake_progress &) {});
    }

    template<typename Progress>
    task<> handshake(std::string_view host, Progress progress)
    {
        auto notify = [&progress](handshake_progress step) -> task<> {
            using result_type =
                std::invoke_result_t<Progress &, const handshake_progress &>;
            if constexpr (nxtrt::is_task_v<result_type>) {
                co_await std::invoke(progress, step);
            } else {
                std::invoke(progress, step);
            }
        };
        auto span = [&](std::string_view name, task_factory auto body) {
            return with_handshake_progress_span(notify, name, std::move(body));
        };
        auto event = [&](std::string_view name) -> task<> {
            co_await notify(
                handshake_progress{
                    .kind = handshake_progress_kind::event,
                    .name = name,
                });
        };

        auto hello = co_await span("client_hello.make", [&] {
            return ready_task(nxt::tls::make_tls13_client_hello(host));
        });
        co_await span("client_hello.write", [&] {
            return nxtrt::write_all(writer_, hello.record);
        });

        auto record = co_await span("server_hello.read", [&] {
            return nxt::tls::read_tls_record(reader_);
        });
        auto server_hello = co_await span("server_hello.parse", [&] {
            return ready_task(nxt::tls::parse_tls13_server_hello(record));
        });
        auto shared_secret = co_await span("x25519.shared_secret", [&] {
            auto secret = nxt::crypto::x25519_dh(
                hello.key_pair.secret_key, server_hello.key_share);
            nxt::tls::require_tls(
                secret.has_value(), "X25519 shared secret failed");
            return ready_task(std::move(secret));
        });

        auto transcript =
            nxt::tls::join_bytes(hello.handshake, server_hello.handshake);
        auto handshake_keys = co_await span("keys.derive_handshake", [&] {
            return ready_task(
                nxt::tls::derive_tls13_handshake_keys(
                    *shared_secret, transcript));
        });

        auto leaf_public_key = std::optional<nxt::tls::leaf_public_key>{};
        auto saw_server_finished = false;
        while (!saw_server_finished) {
            record = co_await span("handshake_record.read", [&] {
                return nxt::tls::read_tls_record(reader_);
            });
            if (record.type == 20) {
                co_await event("change_cipher_spec");
                continue;
            }

            auto plaintext = co_await span("handshake_record.decrypt", [&] {
                return ready_task(
                    nxt::tls::open_tls13_record(
                        handshake_keys.server, record));
            });
            if (plaintext.inner_type != 22) {
                co_await event("non_handshake_record");
                continue;
            }

            for (auto const & message :
                 nxt::tls::split_handshake_messages(plaintext.content)) {
                auto type = std::to_integer<std::uint8_t>(message[0]);
                if (type == 11) {
                    co_await span("certificate.parse", [&]() -> task<> {
                        auto cert =
                            nxt::tls::parse_tls13_certificate(message);
                        leaf_public_key =
                            nxt::tls::extract_leaf_public_key(
                                cert.leaf_der);
                        co_return;
                    });
                } else if (type == 15) {
                    co_await span("certificate.verify", [&]() -> task<> {
                        nxt::tls::require_tls(
                            leaf_public_key.has_value(),
                            "certificate_verify arrived before certificate");
                        auto cert_verify =
                            nxt::tls::parse_tls13_certificate_verify(message);
                        auto ok = nxt::tls::verify_certificate_verify(
                            *leaf_public_key, transcript, cert_verify);
                        nxt::tls::require_tls(
                            ok, "CertificateVerify signature failed");
                        co_return;
                    });
                } else if (type == 20) {
                    co_await span("server_finished.verify", [&]() -> task<> {
                        auto received =
                            nxt::tls::parse_tls13_finished(message);
                        auto ok = nxt::tls::verify_finished(
                            handshake_keys.server.traffic_secret,
                            transcript,
                            received);
                        nxt::tls::require_tls(ok, "server Finished failed");
                        saw_server_finished = true;
                        co_return;
                    });
                }
                nxt::tls::put_bytes(transcript, message);
            }
        }

        application_keys_ = co_await span("keys.derive_application", [&] {
            return ready_task(
                nxt::tls::derive_tls13_application_keys(
                    handshake_keys.secret, transcript));
        });
        auto client_finished = nxt::tls::make_finished_message(
            handshake_keys.client.traffic_secret, transcript);
        co_await span("client_finished.write", [&]() -> task<> {
            auto record = nxt::tls::seal_tls13_record(
                handshake_keys.client, 22, client_finished);
            co_await nxtrt::write_all(writer_, record);
        });
        nxt::tls::put_bytes(transcript, client_finished);
        handshaken_ = true;
        co_await event("ready");
    }

    task<> write_all(std::span<const std::byte> bytes)
    {
        require_handshake();
        co_await nxtrt::write_all(writer_,
            nxt::tls::seal_tls13_record(
                application_keys_.client, 23, bytes));
    }

    task<> write_all(std::string_view text)
    {
        co_await write_all(nxtrt::as_bytes(text));
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

private:
    hope<fare_t> stream_more(
        bytesink & writer,
        std::size_t limit) override
    {
        require_handshake();
        if (limit == 0)
            return hope<fare_t>::ready(0);

        if (pending_offset_ < pending_.size())
            return copy_pending(writer, limit);

        return stream_more_task(writer, limit);
    }

    task<fare_t> stream_more_task(
        bytesink & writer,
        std::size_t limit)
    {
        pending_.clear();
        pending_offset_ = 0;
        do {
            auto plaintext = co_await read();
            if (plaintext.inner_type == 23) {
                pending_ = std::move(plaintext.content);
                break;
            }
        } while (pending_.empty());

        co_return co_await copy_pending(writer, limit);
    }

    hope<fare_t> copy_pending(
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
            return hope<fare_t>::ready(
                n);
        }
        return copy_pending_slow(std::move(write), n);
    }

    task<fare_t> copy_pending_slow(
        hope<void> write,
        std::size_t n)
    {
        co_await std::move(write);
        pending_offset_ += n;
        co_return n;
    }

    void require_handshake() const
    {
        if (!handshaken_)
            throw runtime_error{"TLS session has not completed handshake"};
    }

    bytefeed & reader_;
    bytesink & writer_;
    nxt::tls::tls13_application_keys application_keys_;
    std::vector<std::byte> pending_;
    std::size_t pending_offset_ = 0;
    bool handshaken_ = false;
};

} // namespace nxtrt::tls
