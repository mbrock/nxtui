#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

namespace nxtrt::alloc_trace {

/// Read once: tracing is chosen at process start, and events fire on hot
/// allocation paths where a getenv call per event dominated the cost.
inline bool enabled() noexcept
{
    static const bool on = [] {
        auto const * value = std::getenv("NXT_ALLOC_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

// The pointer is logged as an address; its storage need not be initialized.
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::access(none, 3)]]
#endif
inline void event(
    const char * source,
    const char * action,
    const void * ptr,
    std::size_t size = 0,
    std::size_t alignment = 0,
    std::size_t used = 0,
    std::size_t capacity = 0) noexcept
{
    if (!enabled())
        return;

    static thread_local bool tracing = false;
    if (tracing)
        return;

    tracing = true;
    char line[256];
    auto n = std::snprintf(
        line,
        sizeof(line),
        "%-3s %-12s %10zu %4zu %p %10zu/%-10zu\n",
        action,
        source,
        size,
        alignment,
        ptr,
        used,
        capacity);
    if (n > 0) {
        auto len = static_cast<std::size_t>(n);
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        // Tracing is best effort: do not retry or allocate on this hot path.
        const auto written = ::write(STDERR_FILENO, line, len);
        (void)written;
    }
    tracing = false;
}

} // namespace nxtrt::alloc_trace
