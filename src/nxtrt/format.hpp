#pragma once

// libc++'s std::format is header-only: every translation unit that calls it
// compiles the whole formatting engine (integer and float conversion,
// escaping, replacement fields). nxtrt::format keeps the compile-time format
// check but hands the arguments to one vformat compiled in nxt-core.

#include <format>
#include <string>
#include <string_view>

namespace nxtrt {

/// std::vformat, compiled once in nxt-core.
[[nodiscard]] std::string vformat(std::string_view fmt, std::format_args args);

template<typename... Args>
[[nodiscard]] std::string
format(std::format_string<Args...> fmt, Args &&... args)
{
    return nxtrt::vformat(fmt.get(), std::make_format_args(args...));
}

} // namespace nxtrt
