#pragma once

#include <nxt/crypto.hpp>
#include <nxt/mt/encrypted_packet.hpp>
#include <nxt/mt/message.hpp>
#include <nxt/mt/tl.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace nxt::mt {

inline constexpr std::uint32_t ping_constructor = 0x7abe77ec;
inline constexpr std::uint32_t pong_constructor = 0x347773c5;
inline constexpr std::uint32_t msgs_ack_constructor = 0x62d6b459;
inline constexpr std::uint32_t new_session_created_constructor = 0x9ec20908;
inline constexpr std::uint32_t bad_server_salt_constructor = 0xedab447b;
inline constexpr std::uint32_t rpc_result_constructor = 0xf35c6d01;
inline constexpr std::uint32_t msg_container_constructor = 0x73f1f8dc;

struct session_message_sent
{
    std::int64_t message_id = 0;
    std::int32_t seq_no = 0;
    bytes body;
};

struct session_message_received
{
    std::int64_t message_id = 0;
    std::int32_t seq_no = 0;
    bytes body;
};

struct pong_received
{
    std::int64_t message_id = 0;
    std::int64_t ping_id = 0;
};

struct msgs_ack_received
{
    std::vector<std::int64_t> message_ids;
};

struct msgs_ack_sent
{
    std::int64_t message_id = 0;
    std::vector<std::int64_t> message_ids;
};

struct new_session_created
{
    std::int64_t first_msg_id = 0;
    std::int64_t unique_id = 0;
    std::int64_t server_salt = 0;
};

struct bad_server_salt
{
    std::int64_t bad_msg_id = 0;
    std::int32_t bad_msg_seq_no = 0;
    std::int32_t error_code = 0;
    std::int64_t new_server_salt = 0;
};

struct rpc_result
{
    std::int64_t request_msg_id = 0;
    bytes result;
};

struct rpc_request_result
{
    std::int64_t request_msg_id = 0;
    std::string request;
    bytes result;
};

struct unhandled_session_message
{
    std::int64_t message_id = 0;
    std::int32_t seq_no = 0;
    bytes body;
};

using session_event = std::variant<
    session_message_sent,
    session_message_received,
    pong_received,
    msgs_ack_received,
    msgs_ack_sent,
    new_session_created,
    bad_server_salt,
    rpc_result,
    rpc_request_result,
    unhandled_session_message>;

struct send_encrypted
{
    bytes payload;
};

struct notify_session
{
    session_event event;
};

using session_effect = std::variant<send_encrypted, notify_session>;

struct pending_request
{
    bytes body;
    std::string request;
    std::int32_t seq_no = 0;
};

struct session
{
    auth_key key;
    std::int64_t server_salt = 0;
    std::int64_t session_id = 0;
    std::int64_t time_offset = 0;
    std::optional<std::uint64_t> last_message_id;
    std::uint64_t sent_content_messages = 0;
    std::map<std::int64_t, pending_request> pending_requests;
};

struct send_request_result
{
    session next;
    std::int64_t request_id = 0;
    std::vector<session_effect> effects;
};

struct receive_packet_result
{
    session next;
    std::vector<session_effect> effects;
};

inline bytes encode_msgs_ack_body(std::span<const std::int64_t> message_ids)
{
    auto out = bytes{};
    auto header = std::array<std::byte, 8>{};
    auto writer = byte_writer{header};
    writer.put_le(msgs_ack_constructor, 4);
    writer.put_le(tl::vector_constructor, 4);
    out.insert(out.end(), writer.written().begin(), writer.written().end());

    auto count = std::array<std::byte, 4>{};
    writer = byte_writer{count};
    writer.put_le(static_cast<std::uint32_t>(message_ids.size()), 4);
    out.insert(out.end(), count.begin(), count.end());
    for (auto id : message_ids) {
        auto storage = std::array<std::byte, 8>{};
        writer = byte_writer{storage};
        writer.put_le(static_cast<std::uint64_t>(id), 8);
        out.insert(out.end(), storage.begin(), storage.end());
    }
    return out;
}

inline std::uint64_t adjusted_now_nanoseconds(
    const session & state,
    std::uint64_t now_ns)
{
    constexpr auto ns_per_second = std::int64_t{1'000'000'000};
    auto adjusted = static_cast<__int128>(now_ns)
                  + static_cast<__int128>(state.time_offset) * ns_per_second;
    if (adjusted < 0)
        return 0;
    if (adjusted > std::numeric_limits<std::uint64_t>::max())
        return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(adjusted);
}

inline std::int32_t next_session_seq_no(
    const session & state,
    bool content_related)
{
    return static_cast<std::int32_t>(state.sent_content_messages * 2)
           + (content_related ? 1 : 0);
}

inline encrypted_packet make_session_packet(
    const session & state,
    std::uint64_t now_ns,
    bytes body,
    bool content_related)
{
    return encrypted_packet{
        .salt = state.server_salt,
        .session_id = state.session_id,
        .message_id = static_cast<std::int64_t>(next_message_id(
            state.last_message_id,
            adjusted_now_nanoseconds(state, now_ns))),
        .seq_no = next_session_seq_no(state, content_related),
        .body = std::move(body),
        .padding = {},
    };
}

inline void advance_outbound_session(
    session & state,
    std::int64_t message_id,
    bool content_related)
{
    state.last_message_id = static_cast<std::uint64_t>(message_id);
    if (content_related)
        ++state.sent_content_messages;
}

inline void append_send_effects(
    std::vector<session_effect> & effects,
    bytes encrypted,
    const encrypted_packet & packet)
{
    effects.push_back(send_encrypted{.payload = std::move(encrypted)});
    effects.push_back(notify_session{.event = session_message_sent{
                                         .message_id = packet.message_id,
                                         .seq_no = packet.seq_no,
                                         .body = packet.body,
                                     }});
}

inline send_request_result send_request(
    session state,
    std::uint64_t now_ns,
    bytes body,
    std::string request,
    std::span<const std::byte> padding_bytes)
{
    auto packet = make_session_packet(state, now_ns, std::move(body), true);
    auto encrypted = encode_encrypted_packet(
        packet,
        state.key,
        sender::client,
        padding_bytes);
    auto finalized = decode_encrypted_packet(
        encrypted,
        state.key,
        sender::client,
        state.session_id);
    advance_outbound_session(state, finalized.message_id, true);
    state.pending_requests.emplace(
        finalized.message_id,
        pending_request{
            .body = finalized.body,
            .request = std::move(request),
            .seq_no = finalized.seq_no,
        });

    auto effects = std::vector<session_effect>{};
    append_send_effects(effects, std::move(encrypted), finalized);
    return {
        .next = std::move(state),
        .request_id = finalized.message_id,
        .effects = std::move(effects),
    };
}

inline void handle_session_message(
    session & state,
    std::int64_t message_id,
    std::int32_t seq_no,
    std::span<const std::byte> body,
    std::vector<session_effect> & effects);

inline void handle_session_message(
    session & state,
    std::int64_t message_id,
    std::int32_t seq_no,
    std::span<const std::byte> body,
    std::vector<session_effect> & effects)
{
    auto input = byte_reader{body};
    auto constructor = input.u32_le();

    if (constructor == pong_constructor) {
        auto msg_id = input.i64_le();
        auto ping_id = input.i64_le();
        if (!input.empty())
            throw protocol_error{"trailing pong bytes"};
        effects.push_back(notify_session{.event = pong_received{
                                             .message_id = msg_id,
                                             .ping_id = ping_id,
                                         }});
        return;
    }

    if (constructor == msgs_ack_constructor) {
        if (input.u32_le() != tl::vector_constructor)
            throw protocol_error{"invalid msgs_ack vector"};
        auto count = input.u32_le();
        auto ids = std::vector<std::int64_t>{};
        ids.reserve(count);
        for (auto i = std::uint32_t{0}; i < count; i++)
            ids.push_back(input.i64_le());
        if (!input.empty())
            throw protocol_error{"trailing msgs_ack bytes"};
        effects.push_back(notify_session{.event = msgs_ack_received{
                                             .message_ids = std::move(ids),
                                         }});
        return;
    }

    if (constructor == new_session_created_constructor) {
        auto first_msg_id = input.i64_le();
        auto unique_id = input.i64_le();
        auto server_salt = input.i64_le();
        if (!input.empty())
            throw protocol_error{"trailing new_session_created bytes"};
        state.server_salt = server_salt;
        effects.push_back(notify_session{.event = new_session_created{
                                             .first_msg_id = first_msg_id,
                                             .unique_id = unique_id,
                                             .server_salt = server_salt,
                                         }});
        return;
    }

    if (constructor == bad_server_salt_constructor) {
        auto bad_msg_id = input.i64_le();
        auto bad_msg_seq_no = input.i32_le();
        auto error_code = input.i32_le();
        auto new_server_salt = input.i64_le();
        if (!input.empty())
            throw protocol_error{"trailing bad_server_salt bytes"};
        state.server_salt = new_server_salt;
        effects.push_back(notify_session{.event = bad_server_salt{
                                             .bad_msg_id = bad_msg_id,
                                             .bad_msg_seq_no = bad_msg_seq_no,
                                             .error_code = error_code,
                                             .new_server_salt = new_server_salt,
                                         }});
        return;
    }

    if (constructor == rpc_result_constructor) {
        auto request_msg_id = input.i64_le();
        auto result = input.remaining();
        auto result_bytes = bytes{result.begin(), result.end()};
        auto found = state.pending_requests.find(request_msg_id);
        auto request = std::optional<std::string>{};
        if (found != state.pending_requests.end()) {
            request = std::move(found->second.request);
            state.pending_requests.erase(found);
        }
        effects.push_back(notify_session{.event = rpc_result{
                                             .request_msg_id = request_msg_id,
                                             .result = result_bytes,
                                         }});
        if (request) {
            effects.push_back(notify_session{.event = rpc_request_result{
                                                 .request_msg_id = request_msg_id,
                                                 .request = std::move(*request),
                                                 .result = std::move(result_bytes),
                                             }});
        }
        return;
    }

    if (constructor == msg_container_constructor) {
        auto count = input.i32_le();
        if (count < 0)
            throw protocol_error{"invalid message container count"};
        for (auto i = std::int32_t{0}; i < count; i++) {
            auto child_msg_id = input.i64_le();
            auto child_seq_no = input.i32_le();
            auto body_size = input.i32_le();
            if (body_size < 0)
                throw protocol_error{"invalid container message length"};
            auto child_body = input.take(static_cast<std::size_t>(body_size));
            effects.push_back(notify_session{.event = session_message_received{
                                                 .message_id = child_msg_id,
                                                 .seq_no = child_seq_no,
                                                 .body = bytes{
                                                     child_body.begin(),
                                                     child_body.end()},
                                             }});
            handle_session_message(
                state,
                child_msg_id,
                child_seq_no,
                child_body,
                effects);
        }
        if (!input.empty())
            throw protocol_error{"trailing message container bytes"};
        return;
    }

    effects.push_back(notify_session{.event = unhandled_session_message{
                                         .message_id = message_id,
                                         .seq_no = seq_no,
                                         .body = bytes{body.begin(), body.end()},
                                     }});
}

inline void send_session_ack(
    session & state,
    std::uint64_t now_ns,
    std::span<const std::int64_t> message_ids,
    std::vector<session_effect> & effects,
    std::span<const std::byte> padding_bytes)
{
    auto packet = make_session_packet(
        state,
        now_ns,
        encode_msgs_ack_body(message_ids),
        false);
    auto encrypted = encode_encrypted_packet(
        packet,
        state.key,
        sender::client,
        padding_bytes);
    auto finalized = decode_encrypted_packet(
        encrypted,
        state.key,
        sender::client,
        state.session_id);
    advance_outbound_session(state, finalized.message_id, false);
    append_send_effects(effects, std::move(encrypted), finalized);
    effects.push_back(notify_session{.event = msgs_ack_sent{
                                         .message_id = finalized.message_id,
                                         .message_ids = std::vector<std::int64_t>{
                                             message_ids.begin(),
                                             message_ids.end()},
                                     }});
}

inline receive_packet_result receive_packet(
    session state,
    std::span<const std::byte> payload,
    std::uint64_t now_ns,
    std::span<const std::byte> padding_bytes)
{
    auto packet = decode_encrypted_packet(
        payload,
        state.key,
        sender::server,
        state.session_id);

    auto effects = std::vector<session_effect>{};
    effects.push_back(notify_session{.event = session_message_received{
                                         .message_id = packet.message_id,
                                         .seq_no = packet.seq_no,
                                         .body = packet.body,
                                     }});
    handle_session_message(
        state,
        packet.message_id,
        packet.seq_no,
        packet.body,
        effects);

    if (packet.seq_no % 2 != 0) {
        auto ids = std::array<std::int64_t, 1>{packet.message_id};
        send_session_ack(state, now_ns, ids, effects, padding_bytes);
    }

    return {.next = std::move(state), .effects = std::move(effects)};
}

} // namespace nxt::mt
