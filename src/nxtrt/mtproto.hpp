#pragma once

#include "nxtrt/buffers.hpp"
#include "nxtrt/net.hpp"
#include "nxtrt/net_dns.hpp"
#include "nxtrt/task.hpp"
#include "nxt/crypto.hpp"
#include "nxt/mt/auth.hpp"
#include "nxt/mt/message.hpp"
#include "nxt/mt/session.hpp"
#include "nxt/mt/telegram.hpp"
#include "nxt/mt/tl.hpp"
#include "nxt/mt/transport.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nxtrt::mtproto {

namespace detail {

inline std::uint8_t abridged_u8(
    byte_chunks<const std::byte> bytes,
    std::size_t index)
{
    for (auto chunk : bytes) {
        if (index < chunk.size())
            return std::to_integer<std::uint8_t>(chunk[index]);
        index -= chunk.size();
    }
    throw nxt::mt::protocol_error{"short abridged frame"};
}

inline std::uint64_t abridged_le(
    byte_chunks<const std::byte> bytes,
    std::size_t offset,
    std::size_t width)
{
    if (width > 8)
        throw nxt::mt::protocol_error{"little-endian integer is too wide"};

    auto value = std::uint64_t{0};
    for (auto i = std::size_t{0}; i < width; i++) {
        value |= static_cast<std::uint64_t>(abridged_u8(bytes, offset + i))
              << (i * 8);
    }
    return value;
}

} // namespace detail

struct abridged_frame_view
{
    std::optional<std::uint32_t> quick_ack_token;
    byte_chunks<const std::byte> payload;

    [[nodiscard]] bool is_quick_ack() const noexcept
    {
        return quick_ack_token.has_value();
    }

    [[nodiscard]] bool is_payload() const noexcept
    {
        return !quick_ack_token.has_value();
    }

    static chop_scan_result<abridged_frame_view> scan(
        byte_chunks<const std::byte> bytes)
    {
        if (bytes.size() < 1)
            return chop_need_more{.minimum_buffered = 1};

        auto first = detail::abridged_u8(bytes, 0);
        if (first >= 0x80) {
            if (bytes.size() < 4)
                return chop_need_more{.minimum_buffered = 4};

            auto raw = static_cast<std::uint32_t>(first)
                     | (static_cast<std::uint32_t>(
                            detail::abridged_u8(bytes, 1))
                        << 8)
                     | (static_cast<std::uint32_t>(
                            detail::abridged_u8(bytes, 2))
                        << 16)
                     | (static_cast<std::uint32_t>(
                            detail::abridged_u8(bytes, 3))
                        << 24);
            return frame_chop<abridged_frame_view>{
                .extent = 4,
                .frame = abridged_frame_view{
                    .quick_ack_token = nxt::mt::byte_swap32(raw),
                    .payload = {},
                },
            };
        }

        auto header = std::size_t{1};
        auto words = std::size_t{first};
        if (first == 0x7f) {
            if (bytes.size() < 4)
                return chop_need_more{.minimum_buffered = 4};

            words = static_cast<std::size_t>(
                detail::abridged_u8(bytes, 1)
                | (static_cast<std::uint32_t>(
                       detail::abridged_u8(bytes, 2))
                   << 8)
                | (static_cast<std::uint32_t>(
                       detail::abridged_u8(bytes, 3))
                   << 16));
            header = 4;
        }

        if (words >= nxt::mt::abridged_max_words)
            throw nxt::mt::protocol_error{"abridged payload is too large"};

        auto payload_size = words * 4;
        auto extent = header + payload_size;
        if (bytes.size() < extent)
            return chop_need_more{.minimum_buffered = extent};

        return frame_chop<abridged_frame_view>{
            .extent = extent,
            .frame = abridged_frame_view{
                .quick_ack_token = std::nullopt,
                .payload = bytes.subspan(header, payload_size),
            },
        };
    }
};

using abridged_reel = reel<std::byte, abridged_frame_view>;

struct plain_abridged_frame_view
{
    std::optional<std::uint32_t> quick_ack_token;
    std::uint64_t message_id = 0;
    byte_chunks<const std::byte> body;

    [[nodiscard]] bool is_quick_ack() const noexcept
    {
        return quick_ack_token.has_value();
    }

    [[nodiscard]] bool is_message() const noexcept
    {
        return !quick_ack_token.has_value();
    }

    static chop_scan_result<plain_abridged_frame_view> scan(
        byte_chunks<const std::byte> bytes)
    {
        auto transport = abridged_frame_view::scan(bytes);
        auto * complete = std::get_if<frame_chop<abridged_frame_view>>(
            &transport);
        if (complete == nullptr)
            return std::get<chop_need_more>(transport);

        if (complete->frame.is_quick_ack()) {
            return frame_chop<plain_abridged_frame_view>{
                .extent = complete->extent,
                .frame = plain_abridged_frame_view{
                    .quick_ack_token = complete->frame.quick_ack_token,
                    .message_id = 0,
                    .body = {},
                },
            };
        }

        auto payload = complete->frame.payload;
        if (payload.size() < 20)
            throw nxt::mt::protocol_error{"short plain MTProto message"};
        if (detail::abridged_le(payload, 0, 8) != 0)
            throw nxt::mt::protocol_error{
                "encrypted message is not a plain message",
            };

        auto body_size = static_cast<std::size_t>(
            detail::abridged_le(payload, 16, 4));
        auto body_offset = std::size_t{20};
        if (body_size > payload.size() - body_offset)
            throw nxt::mt::protocol_error{"short plain MTProto body"};
        if (payload.size() != body_offset + body_size)
            throw nxt::mt::protocol_error{"trailing plain message bytes"};

        return frame_chop<plain_abridged_frame_view>{
            .extent = complete->extent,
            .frame = plain_abridged_frame_view{
                .quick_ack_token = std::nullopt,
                .message_id = detail::abridged_le(payload, 8, 8),
                .body = payload.subspan(body_offset, body_size),
            },
        };
    }
};

using plain_abridged_reel = reel<std::byte, plain_abridged_frame_view>;

struct auth_session
{
    nxt::mt::auth_key key;
    std::int64_t server_salt = 0;
    std::int64_t session_id = 0;
    std::int64_t time_offset = 0;
};

class chunk_reader
{
public:
    explicit chunk_reader(byte_chunks<const std::byte> input)
        : input_(input)
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return offset_ == input_.size();
    }

    [[nodiscard]] std::size_t remaining_size() const noexcept
    {
        return input_.size() - offset_;
    }

    [[nodiscard]] byte_chunks<const std::byte> remaining() const
    {
        return input_.subspan(offset_);
    }

    byte_chunks<const std::byte> take(std::size_t count)
    {
        if (count > remaining_size())
            throw nxt::mt::protocol_error{"short MTProto input"};
        auto out = input_.subspan(offset_, count);
        offset_ += count;
        return out;
    }

    std::uint8_t u8()
    {
        return detail::abridged_u8(take(1), 0);
    }

    std::uint64_t le(std::size_t width)
    {
        auto input = take(width);
        return detail::abridged_le(input, 0, width);
    }

    std::uint32_t u32_le()
    {
        return static_cast<std::uint32_t>(le(4));
    }

    std::uint64_t u64_le()
    {
        return le(8);
    }

    std::int32_t i32_le()
    {
        return std::bit_cast<std::int32_t>(u32_le());
    }

    std::int64_t i64_le()
    {
        return std::bit_cast<std::int64_t>(u64_le());
    }

private:
    byte_chunks<const std::byte> input_;
    std::size_t offset_ = 0;
};

class tl_chunk_reader
{
public:
    explicit tl_chunk_reader(byte_chunks<const std::byte> input)
        : input_(input)
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return input_.empty();
    }

    [[nodiscard]] byte_chunks<const std::byte> remaining() const
    {
        return input_.remaining();
    }

    std::int32_t int_()
    {
        return input_.i32_le();
    }

    std::uint64_t long_()
    {
        return input_.u64_le();
    }

    std::int64_t signed_long()
    {
        return input_.i64_le();
    }

    bool bool_()
    {
        auto constructor = input_.u32_le();
        if (constructor == nxt::mt::tl::bool_true)
            return true;
        if (constructor == nxt::mt::tl::bool_false)
            return false;
        throw nxt::mt::protocol_error{"invalid TL bool"};
    }

    byte_chunks<const std::byte> int128()
    {
        return input_.take(16);
    }

    byte_chunks<const std::byte> int256()
    {
        return input_.take(32);
    }

    byte_chunks<const std::byte> bytes()
    {
        auto size = std::size_t{input_.u8()};
        auto header = std::size_t{1};
        if (size == 254) {
            size = input_.le(3);
            header = 4;
        }

        auto value = input_.take(size);
        input_.take(nxt::mt::tl_padding_size(header + size));
        return value;
    }

private:
    chunk_reader input_;
};

inline task<void> write_abridged_client_prefix(bytesink & writer)
{
    auto prefix = std::array{nxt::mt::abridged_client_prefix};
    co_await writer.write(std::span<const std::byte>{prefix});
    co_await writer.flush();
}

inline task<void> write_abridged_frame(
    bytesink & writer,
    std::span<const std::byte> payload,
    bool quick_ack_requested = false)
{
    auto header = std::array<std::byte, 4>{};
    auto header_writer = nxt::mt::byte_writer{header};
    auto words = payload.size() / 4;
    (void)nxt::mt::abridged_frame_size(payload);
    if (words < 0x7f) {
        header_writer.put_u8(static_cast<std::uint8_t>(
            words + (quick_ack_requested ? 0x80 : 0)));
    } else {
        header_writer.put_u8(quick_ack_requested ? 0xff : 0x7f);
        header_writer.put_le(words, 3);
    }

    auto spans = std::array{
        std::span<const std::byte>{header_writer.written()},
        payload,
    };
    auto chunks = byte_chunks<const std::byte, 2>{std::span{spans}};
    co_await writer.write(chunks);
    co_await writer.flush();
}

template<typename Reader>
task<std::span<const std::byte>> read_abridged_frame(Reader & reader)
{
    while (true) {
        auto first_byte = co_await reader.take(1);
        auto first = std::to_integer<std::uint8_t>(first_byte[0]);
        if (first >= 0x80) {
            (void)co_await reader.take(3);
            continue;
        }

        auto words = std::size_t{first};
        if (first == 0x7f) {
            auto extended = co_await reader.take(3);
            words = static_cast<std::size_t>(
                std::to_integer<std::uint8_t>(extended[0])
                | (static_cast<std::uint32_t>(
                       std::to_integer<std::uint8_t>(extended[1]))
                   << 8)
                | (static_cast<std::uint32_t>(
                       std::to_integer<std::uint8_t>(extended[2]))
                   << 16));
        }

        co_return co_await reader.take(words * 4);
    }
}

inline task<void> write_plain_abridged_frame(
    bytesink & writer,
    std::span<const std::byte> body,
    std::optional<std::uint64_t> & last_message_id,
    std::span<std::byte> storage,
    std::uint64_t now_ns = nxt::mt::now_nanoseconds())
{
    auto message_size = nxt::mt::plain_message_size(body);
    if (storage.size() < message_size)
        throw nxt::mt::protocol_error{"plain message storage is too small"};

    auto message_id = nxt::mt::next_message_id(last_message_id, now_ns);
    auto message_writer = nxt::mt::byte_writer{storage.first(message_size)};
    nxt::mt::write_plain_message(
        message_writer,
        nxt::mt::plain_message_view{
            .message_id = message_id,
            .body = body,
        });

    co_await write_abridged_frame(writer, message_writer.written());
    last_message_id = message_id;
}

template<typename Reader>
task<nxt::mt::plain_message_view> read_plain_abridged_frame(Reader & reader)
{
    auto frame = co_await read_abridged_frame(reader);
    co_return nxt::mt::read_plain_message(frame);
}

namespace detail {

inline std::uint8_t hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return static_cast<std::uint8_t>(ch - '0');
    if (ch >= 'a' && ch <= 'f')
        return static_cast<std::uint8_t>(10 + ch - 'a');
    if (ch >= 'A' && ch <= 'F')
        return static_cast<std::uint8_t>(10 + ch - 'A');
    throw nxt::mt::protocol_error{"invalid hex digit"};
}

inline std::array<std::byte, 256> public_key_modulus_from_hex(
    std::string_view hex)
{
    if (hex.size() != 512)
        throw nxt::mt::protocol_error{"invalid public key modulus hex"};

    auto out = std::array<std::byte, 256>{};
    for (auto i = std::size_t{0}; i < out.size(); i++) {
        out[i] = std::byte{
            static_cast<std::uint8_t>(
                (hex_digit(hex[2 * i]) << 4) | hex_digit(hex[2 * i + 1])),
        };
    }
    return out;
}

inline const std::array<std::byte, 256> & telegram_public_key_modulus_b258()
{
    static const auto out = [] {
        constexpr auto hex = std::string_view{
            "c8c11d635691fac091dd9489aedced2932aa8a0bcefef05fa800892d9b52ed03"
            "200865c9e97211cb2ee6c7ae96d3fb0e15aeffd66019b44a08a240cfdd2868"
            "a85e1f54d6fa5deaa041f6941ddf302690d61dc476385c2fa655142353cb4"
            "e4b59f6e5b6584db76fe8b1370263246c010c93d011014113ebdf987d093f"
            "9d37c2be48352d69a1683f8f6e6c2167983c761e3ab169fde5daaa1212"
            "3fa1beab621e4da5935e9c198f82f35eae583a99386d8110ea6bd1abb0"
            "f568759f62694419ea5f69847c43462abef858b4cb5edc84e7b9226cd7"
            "bd7e183aa974a712c079dde85b9dc063b8a5c08e8f859c0ee5dcd824"
            "c7807f20153361a7f63cfd2a433a1be7f5",
        };
        return public_key_modulus_from_hex(hex);
    }();
    return out;
}

inline const std::array<std::byte, 256> & telegram_public_key_modulus_old_main()
{
    static const auto out = [] {
        constexpr auto hex = std::string_view{
            "e8bb3305c0b52c6cf2afdf7637313489e63e05268e5badb601af417786472e5f"
            "93b85438968e20e6729a301c0afc121bf7151f834436f7fda680847a66bf64"
            "accec78ee21c0b316f0edafe2f41908da7bd1f4a5107638eeb67040ace472"
            "a14f90d9f7c2b7def99688ba3073adb5750bb02964902a359fe745d8170e3"
            "6876d4fd8a5d41b2a76cbff9a13267eb9580b2d06d10357448d20d9da21"
            "91cb5d8c93982961cdfdeda629e37f1fb09a0722027696032fe61ed663d"
            "b7a37f6f263d370f69db53a0dc0a1748bdaaff6209d5645485e6e001d1"
            "953255757e4b8e42813347b11da6ab500fd0ace7e6dfa3736199ccaf939"
            "7ed0745a427dcfa6cd67bcb1acff3",
        };
        return public_key_modulus_from_hex(hex);
    }();
    return out;
}

inline std::array<nxt::mt::public_key, 2>
telegram_public_keys(std::span<std::byte> scratch)
{
    auto const & b258 = telegram_public_key_modulus_b258();
    auto const & old_main = telegram_public_key_modulus_old_main();
    return std::array{
        nxt::mt::auth::make_public_key(b258, 65537, scratch),
        nxt::mt::auth::make_public_key(old_main, 65537, scratch),
    };
}

inline std::int32_t now_seconds()
{
    return static_cast<std::int32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

inline std::int64_t random_i64()
{
    auto bytes = std::array<std::byte, 8>{};
    nxt::crypto::random(bytes);
    auto value = std::uint64_t{0};
    for (auto i = std::size_t{0}; i < bytes.size(); i++) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[i]))
                 << (8 * i);
    }
    return std::bit_cast<std::int64_t>(value);
}

} // namespace detail

template<typename Reader>
task<auth_session> perform_auth(bytesink & writer, Reader & reader)
{
    auto state = nxt::mt::auth::exchange_state{};
    auto last_message_id = std::optional<std::uint64_t>{};

    auto nonce = std::array<std::byte, 16>{};
    nxt::crypto::random(nonce);

    auto request_storage = std::array<std::byte, 1024>{};
    auto message_storage = std::array<std::byte, 1200>{};
    auto request_writer = nxt::mt::byte_writer{request_storage};
    nxt::mt::auth::begin(state, nonce, request_writer);
    co_await write_plain_abridged_frame(
        writer,
        request_writer.written(),
        last_message_id,
        message_storage);

    auto res_pq = co_await read_plain_abridged_frame(reader);
    auto random1 = std::array<std::byte, 32 + nxt::mt::rsa_padded_random_size>{};
    nxt::crypto::random(random1);
    auto fingerprint_storage = std::array<std::uint64_t, 8>{};
    auto key_scratch = std::array<std::byte, 264>{};
    auto public_keys = detail::telegram_public_keys(key_scratch);
    auto inner_storage = std::array<std::byte, 256>{};
    auto encrypted_storage = std::array<std::byte, 256>{};
    request_writer = nxt::mt::byte_writer{request_storage};
    nxt::mt::auth::receive_res_pq(
        state,
        res_pq.body,
        public_keys,
        random1,
        fingerprint_storage,
        inner_storage,
        encrypted_storage,
        request_writer);
    co_await write_plain_abridged_frame(
        writer,
        request_writer.written(),
        last_message_id,
        message_storage);

    auto dh_params = co_await read_plain_abridged_frame(reader);
    auto random2 = std::array<std::byte, 512>{};
    nxt::crypto::random(random2);
    auto decrypted = std::array<std::byte, 1024>{};
    auto g_b = std::array<std::byte, 256>{};
    auto auth_key = std::array<std::byte, 256>{};
    auto client_inner = std::array<std::byte, 320>{};
    auto client_encrypted = std::array<std::byte, 384>{};
    request_writer = nxt::mt::byte_writer{request_storage};
    nxt::mt::auth::receive_server_dh_params(
        state,
        dh_params.body,
        random2,
        detail::now_seconds(),
        decrypted,
        g_b,
        auth_key,
        client_inner,
        client_encrypted,
        request_writer);
    co_await write_plain_abridged_frame(
        writer,
        request_writer.written(),
        last_message_id,
        message_storage);

    auto dh_gen = co_await read_plain_abridged_frame(reader);
    nxt::mt::auth::receive_dh_gen(state, dh_gen.body);

    co_return auth_session{
        .key = state.key,
        .server_salt = state.server_salt,
        .session_id = detail::random_i64(),
        .time_offset = state.time_offset,
    };
}

inline task<auth_session> connect_and_auth(
    std::string host = "149.154.167.50",
    std::string service = "443")
{
    auto tx_buffer = std::array<std::byte, 4096>{};
    auto rx_buffer = std::array<std::byte, 8192>{};
    auto socket = net::socket{
        co_await net::connect_tcp(std::move(host), std::move(service)),
        std::span{tx_buffer},
        std::span{rx_buffer},
    };

    co_await write_abridged_client_prefix(socket.output());
    co_return co_await perform_auth(socket.output(), socket.input());
}

inline nxt::mt::session make_session(auth_session auth)
{
    return nxt::mt::session{
        .key = std::move(auth.key),
        .server_salt = auth.server_salt,
        .session_id = auth.session_id,
        .time_offset = auth.time_offset,
        .last_message_id = std::nullopt,
        .sent_content_messages = 0,
        .pending_requests = {},
    };
}

inline task<void> execute_session_effects(
    bytesink & writer,
    const std::vector<nxt::mt::session_effect> & effects)
{
    for (const auto & effect : effects) {
        auto send = std::get_if<nxt::mt::send_encrypted>(&effect);
        if (send != nullptr)
            co_await write_abridged_frame(writer, send->payload);
    }
}

template<typename Reader>
task<std::vector<nxt::mt::session_effect>> receive_next_session_effects(
    bytesink & writer,
    Reader & reader,
    nxt::mt::session & session)
{
    auto frame = co_await read_abridged_frame(reader);
    auto receive_padding = nxt::crypto::random(1024);
    auto received = nxt::mt::receive_packet(
        std::move(session),
        frame,
        nxt::mt::now_nanoseconds(),
        receive_padding);
    session = std::move(received.next);
    co_await execute_session_effects(writer, received.effects);
    co_return std::move(received.effects);
}

template<typename Reader>
task<nxt::mt::bytes> invoke_raw(
    bytesink & writer,
    Reader & reader,
    nxt::mt::session & session,
    nxt::mt::bytes body,
    std::string request_name)
{
    auto padding = nxt::crypto::random(1024);
    auto sent = nxt::mt::send_request(
        std::move(session),
        nxt::mt::now_nanoseconds(),
        std::move(body),
        std::move(request_name),
        padding);
    session = std::move(sent.next);
    auto request_id = sent.request_id;
    co_await execute_session_effects(writer, sent.effects);

    while (true) {
        auto effects =
            co_await receive_next_session_effects(writer, reader, session);

        for (const auto & effect : effects) {
            auto note = std::get_if<nxt::mt::notify_session>(&effect);
            if (note == nullptr)
                continue;
            auto rpc = std::get_if<nxt::mt::rpc_result>(&note->event);
            if (rpc != nullptr && rpc->request_msg_id == request_id)
                co_return rpc->result;
        }
    }
}

} // namespace nxtrt::mtproto
