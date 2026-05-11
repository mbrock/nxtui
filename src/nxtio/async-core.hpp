#pragma once

// Core async primitives - thin wrappers over libcoro.
//
// Internal headers should prefer this file when they only need the
// dependency-light libcoro aliases and helpers. The public facade is
// `nxtio/async.hpp`.

#include <coro/coro.hpp>
#include <coro/event.hpp>
#include <coro/generator.hpp>
#include <coro/latch.hpp>
#include <coro/queue.hpp>
#include <coro/scheduler.hpp>
#include <coro/semaphore.hpp>
#include <coro/task.hpp>
#include <coro/when_any.hpp>

#include <cstddef>
#include <utility>

namespace nxt {

/// Coroutine task type used throughout nxt.
template<typename T = void>
using task = coro::task<T>;

/// Async multi-producer/multi-consumer queue.
template<typename T>
using queue = coro::queue<T>;

/// Coroutine generator type.
template<typename T>
using generator = coro::generator<T>;

/// Counting semaphore with a compile-time maximum value.
template<std::ptrdiff_t max_value>
using semaphore = coro::semaphore<max_value>;

/// Manually signaled coroutine event.
using event = coro::event;
/// Coroutine latch primitive.
using latch = coro::latch;

/// I/O scheduler type used by libcoro transports.
using io_scheduler = coro::scheduler;

/// Poll operation type from libcoro.
using poll_op = coro::poll_op;
/// Poll status type from libcoro.
using poll_status = coro::poll_status;
/// Stop source for cancellable poll operations.
using poll_stop_source = coro::poll_stop_source;

/// Run an awaitable synchronously on the current thread.
inline auto sync_wait(auto && awaitable)
{
    return coro::sync_wait(std::forward<decltype(awaitable)>(awaitable));
}

/// Await a collection of tasks.
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
