#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace nxt::rt {

inline bool trace_env_enabled() noexcept
{
    auto const * raw = std::getenv("NXT_RT_TRACE");
    if (raw == nullptr)
        return false;

    auto value = std::string_view{raw};
    return value == "1" || value == "true" || value == "yes"
        || value == "on";
}

inline bool trace_enabled = trace_env_enabled();

inline void trace(std::string_view message)
{
    if (trace_enabled)
        std::cerr << "[nxt::rt] " << message << '\n';
}

} // namespace nxt::rt
