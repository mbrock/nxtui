#pragma once

#include <atomic>
#include <cstdint>
#include <compare>

namespace nxt::rt {

/// Stable, opaque identity for a coroutine task.
///
/// `0` means "no task". Real tasks start at 1 so an all-zero/default value is
/// a useful sentinel in tests, logs, and optional future registries.
struct task_id
{
    std::uint64_t value = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value != 0;
    }

    friend auto operator<=>(task_id, task_id) = default;
};

class task_id_source
{
public:
    /// Allocate the next process-local task id.
    ///
    /// This is atomic so ids remain unique if future decks or background
    /// workers create coroutine frames from different threads. It does not imply
    /// that the current deck is thread-safe.
    [[nodiscard]] task_id next() noexcept
    {
        return task_id{next_.fetch_add(1, std::memory_order::relaxed)};
    }

private:
    std::atomic_uint64_t next_{1};
};

} // namespace nxt::rt
