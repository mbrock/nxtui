#pragma once

#include <nxt/mt/wire.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace nxt::mt::tl {

inline constexpr std::uint32_t vector_constructor = 0x1cb5c415;
inline constexpr std::uint32_t bool_true = 0x997275b5;
inline constexpr std::uint32_t bool_false = 0xbc799737;

inline std::size_t bytes_size(std::size_t value_size)
{
    auto header = value_size < 254 ? std::size_t{1} : std::size_t{4};
    return header + value_size + mt::tl_padding_size(header + value_size);
}

inline void write_int(byte_writer & out, std::int32_t value)
{
    out.put_le(static_cast<std::uint32_t>(value), 4);
}

inline void write_long(byte_writer & out, std::uint64_t value)
{
    out.put_le(value, 8);
}

inline void write_signed_long(byte_writer & out, std::int64_t value)
{
    out.put_le(static_cast<std::uint64_t>(value), 8);
}

inline void write_bool(byte_writer & out, bool value)
{
    out.put_le(value ? bool_true : bool_false, 4);
}

inline void write_int128(byte_writer & out, std::span<const std::byte> value)
{
    if (value.size() != 16)
        throw protocol_error{"invalid TL int128"};
    out.put(value);
}

inline void write_int256(byte_writer & out, std::span<const std::byte> value)
{
    if (value.size() != 32)
        throw protocol_error{"invalid TL int256"};
    out.put(value);
}

inline void write_bytes(byte_writer & out, std::span<const std::byte> value)
{
    if (value.size() < 254) {
        out.put_u8(static_cast<std::uint8_t>(value.size()));
        out.put(value);
        out.put_zero(mt::tl_padding_size(1 + value.size()));
        return;
    }

    if (value.size() > 0xff'ffff)
        throw protocol_error{"TL bytes value is too large"};
    out.put_u8(254);
    out.put_le(value.size(), 3);
    out.put(value);
    out.put_zero(mt::tl_padding_size(4 + value.size()));
}

class reader
{
public:
    explicit reader(std::span<const std::byte> input)
        : input_(input)
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return input_.empty();
    }

    [[nodiscard]] std::span<const std::byte> remaining() const noexcept
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
        if (constructor == bool_true)
            return true;
        if (constructor == bool_false)
            return false;
        throw protocol_error{"invalid TL bool"};
    }

    std::span<const std::byte> int128()
    {
        return input_.take(16);
    }

    std::span<const std::byte> int256()
    {
        return input_.take(32);
    }

    std::span<const std::byte> bytes()
    {
        auto size = std::size_t{input_.u8()};
        auto header = std::size_t{1};
        if (size == 254) {
            size = input_.le(3);
            header = 4;
        }

        auto value = input_.take(size);
        input_.take(mt::tl_padding_size(header + size));
        return value;
    }

private:
    byte_reader input_;
};

} // namespace nxt::mt::tl
