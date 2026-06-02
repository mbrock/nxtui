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

namespace nxtrt::tls {

enum class tls_event_kind
{
    change_cipher_spec,
    handshake,
    application_data,
    alert,
    unknown,
};

struct tls13_session_event
{
    tls_event_kind kind = tls_event_kind::unknown;
    std::uint8_t content_type = 0;
    // Borrows from the session's current record storage until next_event().
    std::span<std::byte> content;
};

tls_event_kind tls_event_kind_from_content_type(std::uint8_t type);

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

    task<> handshake(std::string_view host);
    task<> write_all(std::span<const std::byte> bytes);
    task<> write_all(std::string_view text);
    task<nxt::tls::tls13_plaintext> read();
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
    std::vector<std::byte> pending_;
    std::size_t pending_offset_ = 0;
    std::uint8_t record_type_ = 0;
    std::uint16_t record_version_ = 0;
    bool handshaken_ = false;
};

} // namespace nxtrt::tls
