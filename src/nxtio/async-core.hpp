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

#include "nxtio/stacktrace.hpp"

#include <cstddef>
#include <exception>
#include <string>
#include <utility>
#include <vector>

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

/// Scheduler type used by nxt async operations.
using scheduler = coro::scheduler;
using io_scheduler = scheduler;

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

/// Invoke a coroutine functor from inside nxt coroutine code while keeping
/// the functor object alive in this wrapper coroutine's frame. Unlike
/// libcoro's coro::invoke, this does not eagerly resume the wrapper before
/// returning it, so it composes with scheduler-owned co_await chains.
template<typename Functor, typename... Args>
auto invoke(Functor functor, Args &&... args)
    -> decltype(functor(std::forward<Args>(args)...))
{
    auto user_task = functor(std::forward<Args>(args)...);
    co_return co_await user_task;
}

class exception_group : public std::exception
{
public:
    explicit exception_group(
        std::string message,
        std::vector<std::exception_ptr> exceptions)
        : message_(std::move(message))
        , exceptions_(std::move(exceptions))
    {
    }

    const char * what() const noexcept override
    {
        return message_.c_str();
    }

    [[nodiscard]] const std::vector<std::exception_ptr> &
    exceptions() const noexcept
    {
        return exceptions_;
    }

private:
    std::string message_;
    std::vector<std::exception_ptr> exceptions_;
};

namespace detail {

inline void collect_when_all_exception(
    auto & completed,
    std::vector<std::exception_ptr> & exceptions)
{
    try {
        completed.return_value();
    } catch (...) {
        exceptions.push_back(std::current_exception());
    }
}

[[noreturn]] inline void throw_when_all_exceptions(
    std::vector<std::exception_ptr> exceptions)
{
    throw exception_group{
        exceptions.size() == 1 ? "one task failed" : "multiple tasks failed",
        std::move(exceptions)};
}

} // namespace detail

/// Await a collection of void tasks, then throw an exception_group if any
/// completed task failed. libcoro stores child exceptions in the returned
/// wrappers, so callers must inspect them; this facade makes that semantic
/// automatic for nxt code.
inline task<> when_all(std::vector<task<>> tasks)
{
    auto completed = co_await coro::when_all(std::move(tasks));
    auto exceptions = std::vector<std::exception_ptr>{};
    for (auto & task : completed)
        detail::collect_when_all_exception(task, exceptions);
    if (!exceptions.empty())
        detail::throw_when_all_exceptions(std::move(exceptions));
}

/// Await several awaitables.
template<typename First, typename Second, typename... Rest>
inline task<> when_all(First && first, Second && second, Rest &&... rest)
{
    auto completed = co_await coro::when_all(
        std::forward<decltype(first)>(first),
        std::forward<decltype(second)>(second),
        std::forward<decltype(rest)>(rest)...);
    auto exceptions = std::vector<std::exception_ptr>{};
    std::apply(
        [&](auto &... task) {
            (detail::collect_when_all_exception(task, exceptions), ...);
        },
        completed);
    if (!exceptions.empty())
        detail::throw_when_all_exceptions(std::move(exceptions));
}

/// Schedule a task to run on the given scheduler.
/// Use this instead of co_await scheduler.schedule() at the top
/// of coroutines.
inline auto start(scheduler & sched, task<> t)
{
    return sched.schedule(std::move(t));
}

} // namespace nxt
