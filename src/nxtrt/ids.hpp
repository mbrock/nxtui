#pragma once

#include <cstdint>
#include <compare>
#include <limits>

namespace nxtrt {

/// Stable, opaque identity for a coroutine task.
///
/// `0` means "no task". Real task ids are compact table identities with a
/// 24-bit one-based row index and an 8-bit era. The exact packing stays behind
/// helpers so traces and backend tickets can treat the id as one 32-bit word.
struct task_id
{
    static constexpr std::uint32_t index_bits = 24;
    static constexpr std::uint32_t index_mask = (1u << index_bits) - 1u;
    static constexpr std::uint32_t max_index = index_mask;

    std::uint32_t value = 0;

    [[nodiscard]] static constexpr task_id make(
        std::uint32_t index,
        std::uint8_t era) noexcept
    {
        return task_id{
            (static_cast<std::uint32_t>(era) << index_bits)
            | (index & index_mask),
        };
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value != 0;
    }

    [[nodiscard]] constexpr std::uint32_t index() const noexcept
    {
        return value & index_mask;
    }

    [[nodiscard]] constexpr std::uint8_t era() const noexcept
    {
        return static_cast<std::uint8_t>(value >> index_bits);
    }

    friend auto operator<=>(task_id, task_id) = default;
};


} // namespace nxtrt
