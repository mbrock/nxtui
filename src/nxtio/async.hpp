#pragma once

// Core async primitives - thin wrappers over libcoro
//
// This file provides:
// - Type aliases for libcoro types (task, queue, event, etc.)
// - Helper functions (sync_wait, when_all, start)
// - task_group for managing concurrent tasks with cancellation
//
// For structured concurrency with channels, see scope.hpp

#include <coro/coro.hpp>
#include <coro/event.hpp>
#include <coro/generator.hpp>
#include <coro/latch.hpp>
#include <coro/queue.hpp>
#include <coro/scheduler.hpp>
#include <coro/semaphore.hpp>
#include <coro/task.hpp>
#include <coro/when_any.hpp>

namespace nxt {

template<typename T = void>
using task = coro::task<T>;

template<typename T>
using queue = coro::queue<T>;

template<typename T>
using generator = coro::generator<T>;

// coro::semaphore<max_value> is a counting semaphore with compile-time max
// Use directly when you know the max at compile time:
//   nxt::semaphore<16> slots;
template<std::ptrdiff_t max_value>
using semaphore = coro::semaphore<max_value>;

using event = coro::event;
using latch = coro::latch;

using io_scheduler = coro::scheduler;

using poll_op = coro::poll_op;
using poll_status = coro::poll_status;
using poll_stop_source = coro::poll_stop_source;

inline auto sync_wait(auto && awaitable)
{
    return coro::sync_wait(std::forward<decltype(awaitable)>(awaitable));
}

inline auto when_all(auto && tasks)
{
    return coro::when_all(std::forward<decltype(tasks)>(tasks));
}

/// Schedule a task to run on the given scheduler.
/// Use this instead of co_await scheduler.schedule() at the top
/// of coroutines.
inline auto start(io_scheduler & sched, task<> t)
{
    return sched.schedule(std::move(t));
}

} // namespace nxt
