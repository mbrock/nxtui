#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

namespace nxtrt::alloc_trace {

inline bool enabled() noexcept
{
    auto const * value = std::getenv("NXT_ALLOC_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

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
        if (len > sizeof(line))
            len = sizeof(line);
        (void)::write(STDERR_FILENO, line, len);
    }
    tracing = false;
}

} // namespace nxtrt::alloc_trace
