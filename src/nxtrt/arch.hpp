#pragma once

#if defined(__linux__)
#include "nxtrt/wand/uring.hpp"
#include "nxtrt/wand/epoll.hpp"
#elif defined(_WIN32)
#include "nxtrt/wand/iocp.hpp"
#else
#include "nxtrt/wand/kqueue.hpp"
#endif

/**
 * @namespace nxtrt::arch
 * The build's default platform wand, chosen at configure time.
 *
 * `arch::wand` is the concrete wand that `nxtrt::runtime` embeds, and
 * `arch::has_wand` (with the macro `NXTRT_ARCH_HAS_WAND`) says whether one
 * exists. Meson's `default_wand` option picks it: `auto` (the default) means
 * epoll under Fil-C, io_uring on other Linux, IOCP on Windows/UWP, and
 * kqueue on macOS and BSD;
 * `uring`, `epoll`, `iocp`, or `kqueue` force one and fail at configure time on an
 * unsupported platform. A forced choice reaches this header and library
 * consumers as `-DNXTRT_DEFAULT_WAND=<name>_wand`; without it the header
 * falls back to the same platform order. See @ref building.
 */
namespace nxtrt::arch {
#if defined(NXTRT_DEFAULT_WAND)
// Meson validates platform support and exports this choice to consumers:
// runtime's concrete Wand is part of its public layout.
#  define NXTRT_ARCH_HAS_WAND 1
/// The default concrete wand for this build (`uring_wand`, `epoll_wand`,
/// `iocp_wand`, or `kqueue_wand`).
using wand = NXTRT_DEFAULT_WAND;
/// True when this platform has a default wand.
inline constexpr bool has_wand = true;
#elif defined(__linux__) && NXT_RT_HAS_URING
#define NXTRT_ARCH_HAS_WAND 1
using wand = uring_wand;
inline constexpr bool has_wand = true;
#elif defined(__linux__) && NXT_RT_HAS_EPOLL
#define NXTRT_ARCH_HAS_WAND 1
using wand = epoll_wand;
inline constexpr bool has_wand = true;
#elif defined(_WIN32)
#define NXTRT_ARCH_HAS_WAND 1
using wand = iocp_wand;
inline constexpr bool has_wand = true;
#elif !defined(__linux__) && NXT_RT_HAS_KQUEUE
#define NXTRT_ARCH_HAS_WAND 1
using wand = kqueue_wand;
inline constexpr bool has_wand = true;
#else
#define NXTRT_ARCH_HAS_WAND 0
inline constexpr bool has_wand = false;
#endif
} // namespace nxtrt::arch
