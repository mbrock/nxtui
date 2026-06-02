#pragma once

#include <nxt/mt/wire.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace nxt::mt {

struct plain_message_view
{
    std::uint64_t message_id = 0;
    std::span<const std::byte> body;
};

inline std::size_t plain_message_size(std::span<const std::byte> body)
{
    return 8 + 8 + 4 + body.size();
}

inline void write_plain_message(byte_writer & out, plain_message_view message)
{
    if (message.body.size() > 0xffff'ffff)
        throw protocol_error{"plain message body is too large"};

    out.put_le(0, 8);
    out.put_le(message.message_id, 8);
    out.put_le(message.body.size(), 4);
    out.put(message.body);
}

inline plain_message_view read_plain_message(std::span<const std::byte> input)
{
    auto reader = byte_reader{input};
    if (reader.u64_le() != 0)
        throw protocol_error{"encrypted message is not a plain message"};

    auto message = plain_message_view{
        .message_id = reader.u64_le(),
        .body = reader.take(reader.u32_le()),
    };
    if (!reader.empty())
        throw protocol_error{"trailing plain message bytes"};
    return message;
}

inline std::uint64_t message_id_from_nanoseconds(std::uint64_t now_ns)
{
    constexpr auto fraction_scale = std::uint64_t{4'294'967'296};
    constexpr auto nanoseconds_per_second = std::uint64_t{1'000'000'000};

    auto seconds = now_ns / nanoseconds_per_second;
    auto nanos = now_ns % nanoseconds_per_second;
    auto fraction = std::max(
        (nanos * fraction_scale) / nanoseconds_per_second,
        std::uint64_t{4});
    auto message_id = seconds * fraction_scale + fraction;
    return message_id - (message_id % 4);
}

inline std::uint64_t next_message_id(
    std::optional<std::uint64_t> last_message_id,
    std::uint64_t now_ns)
{
    auto from_now = message_id_from_nanoseconds(now_ns);
    if (!last_message_id)
        return from_now;
    return std::max(from_now, *last_message_id + 4);
}

inline std::uint64_t now_nanoseconds()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace nxt::mt
