#pragma once

#include <nxt/mt/tl.hpp>
#include <nxt/mt/encrypted_packet.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nxt::mt::telegram {

inline constexpr std::int32_t current_layer = 214;
inline constexpr std::uint32_t invoke_with_layer_constructor = 0xda9b0d0d;
inline constexpr std::uint32_t init_connection_constructor = 0xc1cd5ea9;
inline constexpr std::uint32_t help_get_config_constructor = 0xc4f9186b;
inline constexpr std::uint32_t auth_import_bot_authorization_constructor =
    0x67a3ff2c;
inline constexpr std::uint32_t updates_get_state_constructor = 0xedd4882a;
inline constexpr std::uint32_t updates_get_difference_constructor = 0x19c2f763;
inline constexpr std::uint32_t messages_get_dialogs_constructor = 0xa0f4cb4f;
inline constexpr std::uint32_t input_peer_empty_constructor = 0x7f3b18ea;

inline constexpr std::uint32_t rpc_error_constructor = 0x2144ca19;
inline constexpr std::uint32_t auth_authorization_constructor = 0x2ea2c0d4;
inline constexpr std::uint32_t updates_state_constructor = 0xa56c2a3e;
inline constexpr std::uint32_t updates_difference_empty_constructor =
    0x5d75a138;
inline constexpr std::uint32_t updates_difference_constructor = 0x00f49ca0;
inline constexpr std::uint32_t updates_difference_slice_constructor =
    0xa8fb1981;
inline constexpr std::uint32_t updates_difference_too_long_constructor =
    0x4afe8f6d;
inline constexpr std::uint32_t messages_dialogs_constructor = 0x15ba6c40;
inline constexpr std::uint32_t messages_dialogs_slice_constructor = 0x71e094f3;
inline constexpr std::uint32_t messages_dialogs_not_modified_constructor =
    0xf0e3e596;
inline constexpr std::uint32_t updates_too_long_constructor = 0xe317af7e;
inline constexpr std::uint32_t update_short_message_constructor = 0x313bc7f8;
inline constexpr std::uint32_t update_short_chat_message_constructor =
    0x4d6deea5;
inline constexpr std::uint32_t update_short_constructor = 0x78d4dec1;
inline constexpr std::uint32_t updates_combined_constructor = 0x725b04c3;
inline constexpr std::uint32_t updates_constructor = 0x74ae4240;

struct app_info
{
    std::int32_t api_id = 0;
    std::string device_model = "nxtmt";
    std::string system_version = "nxt";
    std::string app_version = "0.1.0";
    std::string system_lang_code = "en";
    std::string lang_pack;
    std::string lang_code = "en";
    std::int32_t layer = current_layer;
};

struct update_state
{
    std::int32_t pts = 0;
    std::int32_t qts = 0;
    std::int32_t date = 0;
    std::int32_t seq = 0;
    std::int32_t unread_count = 0;
};

struct difference_summary
{
    std::uint32_t constructor = 0;
    std::int32_t date = 0;
    std::int32_t seq = 0;
    std::int32_t pts = 0;
    std::optional<std::int32_t> first_vector_count;
};

inline void append_raw(bytes & out, std::span<const std::byte> input)
{
    out.insert(out.end(), input.begin(), input.end());
}

inline void append_u32(bytes & out, std::uint32_t value)
{
    auto storage = std::array<std::byte, 4>{};
    auto writer = byte_writer{storage};
    writer.put_le(value, 4);
    append_raw(out, storage);
}

inline void append_i32(bytes & out, std::int32_t value)
{
    append_u32(out, static_cast<std::uint32_t>(value));
}

inline void append_i64(bytes & out, std::int64_t value)
{
    auto storage = std::array<std::byte, 8>{};
    auto writer = byte_writer{storage};
    writer.put_le(static_cast<std::uint64_t>(value), 8);
    append_raw(out, storage);
}

inline bytes tl_string(std::string_view text)
{
    auto data = std::as_bytes(std::span<const char>{text.data(), text.size()});
    auto out = bytes(tl::bytes_size(data.size()));
    auto writer = byte_writer{out};
    tl::write_bytes(writer, data);
    return out;
}

inline void append_string(bytes & out, std::string_view text)
{
    append_raw(out, tl_string(text));
}

inline bytes constructor_only(std::uint32_t constructor)
{
    auto out = bytes{};
    append_u32(out, constructor);
    return out;
}

inline bytes help_get_config()
{
    return constructor_only(help_get_config_constructor);
}

inline bytes auth_import_bot_authorization(
    std::int32_t api_id,
    std::string_view api_hash,
    std::string_view bot_token)
{
    if (api_id == 0)
        throw protocol_error{"Telegram api_id is required"};
    if (api_hash.empty())
        throw protocol_error{"Telegram api_hash is required"};
    if (bot_token.empty())
        throw protocol_error{"Telegram bot token is required"};

    auto out = bytes{};
    append_u32(out, auth_import_bot_authorization_constructor);
    append_i32(out, 0);
    append_i32(out, api_id);
    append_string(out, api_hash);
    append_string(out, bot_token);
    return out;
}

inline bytes updates_get_state()
{
    return constructor_only(updates_get_state_constructor);
}

inline bytes updates_get_difference(update_state state)
{
    auto out = bytes{};
    append_u32(out, updates_get_difference_constructor);
    append_i32(out, 0);
    append_i32(out, state.pts);
    append_i32(out, state.date);
    append_i32(out, state.qts);
    return out;
}

inline bytes input_peer_empty()
{
    return constructor_only(input_peer_empty_constructor);
}

inline bytes messages_get_dialogs(std::int32_t limit = 20)
{
    auto out = bytes{};
    append_u32(out, messages_get_dialogs_constructor);
    append_i32(out, 0);
    append_i32(out, 0);
    append_i32(out, 0);
    append_raw(out, input_peer_empty());
    append_i32(out, limit);
    append_i64(out, 0);
    return out;
}

inline bytes init_connection(std::span<const std::byte> query, const app_info & app)
{
    if (app.api_id == 0)
        throw protocol_error{"Telegram api_id is required"};

    auto out = bytes{};
    append_u32(out, init_connection_constructor);
    append_i32(out, 0);
    append_i32(out, app.api_id);
    append_string(out, app.device_model);
    append_string(out, app.system_version);
    append_string(out, app.app_version);
    append_string(out, app.system_lang_code);
    append_string(out, app.lang_pack);
    append_string(out, app.lang_code);
    append_raw(out, query);
    return out;
}

inline bytes invoke_with_layer(std::span<const std::byte> query, std::int32_t layer)
{
    auto out = bytes{};
    append_u32(out, invoke_with_layer_constructor);
    append_i32(out, layer);
    append_raw(out, query);
    return out;
}

inline bytes wrap_request(std::span<const std::byte> query, const app_info & app)
{
    return invoke_with_layer(init_connection(query, app), app.layer);
}

inline std::uint32_t result_constructor(std::span<const std::byte> result)
{
    auto reader = byte_reader{result};
    return reader.u32_le();
}

inline const char * constructor_name(std::uint32_t constructor)
{
    switch (constructor) {
    case rpc_error_constructor: return "rpc_error";
    case auth_authorization_constructor: return "auth.authorization";
    case updates_state_constructor: return "updates.state";
    case updates_difference_empty_constructor: return "updates.differenceEmpty";
    case updates_difference_constructor: return "updates.difference";
    case updates_difference_slice_constructor: return "updates.differenceSlice";
    case updates_difference_too_long_constructor:
        return "updates.differenceTooLong";
    case messages_dialogs_constructor: return "messages.dialogs";
    case messages_dialogs_slice_constructor: return "messages.dialogsSlice";
    case messages_dialogs_not_modified_constructor:
        return "messages.dialogsNotModified";
    case updates_too_long_constructor: return "updatesTooLong";
    case update_short_message_constructor: return "updateShortMessage";
    case update_short_chat_message_constructor: return "updateShortChatMessage";
    case update_short_constructor: return "updateShort";
    case updates_combined_constructor: return "updatesCombined";
    case updates_constructor: return "updates";
    default: return "unknown";
    }
}

inline update_state read_updates_state(std::span<const std::byte> result)
{
    auto reader = byte_reader{result};
    if (reader.u32_le() != updates_state_constructor)
        throw protocol_error{"expected updates.state"};
    auto out = update_state{
        .pts = reader.i32_le(),
        .qts = reader.i32_le(),
        .date = reader.i32_le(),
        .seq = reader.i32_le(),
        .unread_count = reader.i32_le(),
    };
    if (!reader.empty())
        throw protocol_error{"trailing updates.state bytes"};
    return out;
}

inline difference_summary summarize_updates_difference(
    std::span<const std::byte> result)
{
    auto reader = byte_reader{result};
    auto constructor = reader.u32_le();
    auto out = difference_summary{
        .constructor = constructor,
        .date = 0,
        .seq = 0,
        .pts = 0,
        .first_vector_count = std::nullopt,
    };

    if (constructor == updates_difference_empty_constructor) {
        out.date = reader.i32_le();
        out.seq = reader.i32_le();
        if (!reader.empty())
            throw protocol_error{"trailing updates.differenceEmpty bytes"};
        return out;
    }

    if (constructor == updates_difference_too_long_constructor) {
        out.pts = reader.i32_le();
        if (!reader.empty())
            throw protocol_error{"trailing updates.differenceTooLong bytes"};
        return out;
    }

    if (constructor == updates_difference_constructor
        || constructor == updates_difference_slice_constructor) {
        if (reader.u32_le() != tl::vector_constructor)
            throw protocol_error{"invalid updates difference message vector"};
        out.first_vector_count = reader.i32_le();
        return out;
    }

    throw protocol_error{"expected updates.Difference"};
}

struct rpc_error
{
    std::int32_t code = 0;
    std::string message;
};

inline rpc_error read_rpc_error(std::span<const std::byte> result)
{
    auto reader = byte_reader{result};
    if (reader.u32_le() != rpc_error_constructor)
        throw protocol_error{"expected rpc_error"};
    auto out = rpc_error{.code = reader.i32_le(), .message = {}};
    auto tl_reader = tl::reader{reader.remaining()};
    auto message = tl_reader.bytes();
    if (!tl_reader.empty())
        throw protocol_error{"trailing rpc_error bytes"};
    out.message.assign(
        reinterpret_cast<const char *>(message.data()),
        message.size());
    return out;
}

inline std::optional<std::int32_t> migrate_dc_from_error(std::string_view message)
{
    auto marker = std::string_view{"_MIGRATE_"};
    auto pos = message.rfind(marker);
    if (pos == std::string_view::npos)
        return std::nullopt;

    auto suffix = message.substr(pos + marker.size());
    if (suffix.empty())
        return std::nullopt;

    auto dc = std::int32_t{};
    for (auto ch : suffix) {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        dc = dc * 10 + (ch - '0');
    }
    return dc;
}

} // namespace nxt::mt::telegram
