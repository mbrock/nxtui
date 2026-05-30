#pragma once

#if defined(__linux__)
#include "nxtrt/wand/uring.hpp"
#include "nxtrt/wand/epoll.hpp"
#else
#include "nxtrt/wand/kqueue.hpp"
#endif

namespace nxtrt::arch {
#if defined(__linux__) && NXT_RT_HAS_LIBURING
#define NXTRT_ARCH_HAS_WAND 1
using wand = uring_wand;
inline constexpr bool has_wand = true;
#elif defined(__linux__) && NXT_RT_HAS_EPOLL
#define NXTRT_ARCH_HAS_WAND 1
using wand = epoll_wand;
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
