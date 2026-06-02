#pragma once

#include <bit>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace nxt::mt {

struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class byte_writer
{
public:
    explicit byte_writer(std::span<std::byte> output)
        : output_(output)
    {}

    [[nodiscard]] std::size_t size() const noexcept
    {
        return offset_;
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return output_.size() - offset_;
    }

    [[nodiscard]] std::span<std::byte> written() const noexcept
    {
        return output_.first(offset_);
    }

    void put_u8(std::uint8_t value)
    {
        put(std::byte{value});
    }

    void put(std::byte value)
    {
        ensure(1);
        output_[offset_++] = value;
    }

    void put(std::span<const std::byte> input)
    {
        ensure(input.size());
        std::ranges::copy(input, output_.begin() + offset_);
        offset_ += input.size();
    }

    void put_zero(std::size_t count)
    {
        ensure(count);
        std::ranges::fill(output_.subspan(offset_, count), std::byte{0});
        offset_ += count;
    }

    void put_le(std::uint64_t value, std::size_t width)
    {
        if (width > 8)
            throw protocol_error{"little-endian integer is too wide"};
        ensure(width);
        for (std::size_t i = 0; i < width; ++i)
            output_[offset_++] =
                std::byte{static_cast<std::uint8_t>(value >> (i * 8))};
    }

private:
    void ensure(std::size_t count) const
    {
        if (count > remaining())
            throw protocol_error{"MTProto output buffer is too small"};
    }

    std::span<std::byte> output_;
    std::size_t offset_ = 0;
};

class byte_reader
{
public:
    explicit byte_reader(std::span<const std::byte> input)
        : input_(input)
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return input_.empty();
    }

    [[nodiscard]] std::size_t remaining_size() const noexcept
    {
        return input_.size();
    }

    [[nodiscard]] std::span<const std::byte> remaining() const noexcept
    {
        return input_;
    }

    std::span<const std::byte> take(std::size_t count)
    {
        if (count > input_.size())
            throw protocol_error{"short MTProto input"};
        auto out = input_.first(count);
        input_ = input_.subspan(count);
        return out;
    }

    std::uint8_t u8()
    {
        return std::to_integer<std::uint8_t>(take(1)[0]);
    }

    std::uint64_t le(std::size_t width)
    {
        if (width > 8)
            throw protocol_error{"little-endian integer is too wide"};
        auto input = take(width);
        auto value = std::uint64_t{0};
        for (std::size_t i = 0; i < width; ++i) {
            value |= static_cast<std::uint64_t>(
                         std::to_integer<std::uint8_t>(input[i]))
                     << (i * 8);
        }
        return value;
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
    std::span<const std::byte> input_;
};

inline std::size_t tl_padding_size(std::size_t length)
{
    return (4 - (length % 4)) % 4;
}

inline std::uint32_t byte_swap32(std::uint32_t value)
{
    return ((value & 0x000000ffu) << 24)
           | ((value & 0x0000ff00u) << 8)
           | ((value & 0x00ff0000u) >> 8)
           | ((value & 0xff000000u) >> 24);
}

} // namespace nxt::mt
