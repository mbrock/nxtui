#pragma once

#include <cstdint>
#include <compare>
#include <limits>

namespace nxtrt {

/// Identity of a task in its deck's task table, packed in one 32-bit word.
///
/// The value `0` (the default) means "no task"; test with `if (id)`. A real
/// id holds a 24-bit one-based row index and an 8-bit era. The era changes
/// each time a row is reused, so a stale id stops resolving once its task is
/// destroyed (until the 8-bit era wraps around). Ids are meaningful only
/// within the deck that assigned them, and a task has no id until a deck
/// first queues it. Traces print the raw `value`.
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
