#pragma once

#include <iostream>
#include <string_view>

namespace nxt::rt {

inline bool trace_enabled = true;

inline void trace(std::string_view message)
{
    if (trace_enabled)
        std::cerr << "[nxt::rt] " << message << '\n';
}

} // namespace nxt::rt
