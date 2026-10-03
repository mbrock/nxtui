#pragma once

// Sequential task and awaitable composition.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/context.hpp"

namespace nxtrt {

/// A task that awaits `child` without forwarding stop requests to it.
///
/// Stopping the task that awaits the shield does not stop `child` and does
/// not cancel wishes it is parked on, so `child` runs to completion and the
/// shield returns or rethrows its result. Use it for cleanup and other work
/// that must finish even while its owner is being cancelled. `child` can
/// still be stopped directly.
template<typename T>
[[nodiscard]] task<T> shield(task<T> child)
{
    auto awaiter = typename task<T>::awaiter{child.handle(), false};
    if constexpr (std::is_void_v<T>) {
        co_await awaiter;
    } else {
        co_return co_await awaiter;
    }
}

namespace detail {

template<typename T, typename F>
struct then_result
{
    using type = std::invoke_result_t<F &, T>;
};

template<typename F>
struct then_result<void, F>
{
    using type = std::invoke_result_t<F &>;
};

template<typename T, typename F>
using then_result_t = typename then_result<T, F>::type;

template<typename T, typename F>
struct let_value_result
{
    using type = task_result_t<std::invoke_result_t<F &, T>>;
};

template<typename F>
struct let_value_result<void, F>
{
    using type = task_result_t<std::invoke_result_t<F &>>;
};

template<typename T, typename F>
using let_value_result_t = typename let_value_result<T, F>::type;

} // namespace detail

/// A task that awaits `child` and returns `fn(value)` (or `fn()` for a
/// void child).
///
/// `fn` is synchronous and runs on the deck right after `child` finishes.
/// If `child` throws, `fn` is not called and the exception propagates.
/// Unlike `map`, the result is a real `task` that can be stored and handed
/// to groups. Also available as a pipe: `child | then(fn)`.
template<typename T, typename F>
[[nodiscard]] task<detail::then_result_t<T, F>> then(task<T> child, F fn)
{
    using result_type = detail::then_result_t<T, F>;

    if constexpr (std::is_void_v<T>) {
        co_await child;
        if constexpr (std::is_void_v<result_type>) {
            std::invoke(fn);
            co_return;
        } else {
            co_return std::invoke(fn);
        }
    } else {
        auto value = co_await child;
        if constexpr (std::is_void_v<result_type>) {
            std::invoke(fn, std::move(value));
            co_return;
        } else {
            co_return std::invoke(fn, std::move(value));
        }
    }
}

namespace detail {

/// Obtain the awaiter for any awaitable: use `operator co_await` when present,
/// otherwise the value is already an awaiter.
template<typename A>
constexpr decltype(auto) get_awaiter(A && a)
{
    if constexpr (requires { static_cast<A &&>(a).operator co_await(); })
        return static_cast<A &&>(a).operator co_await();
    else if constexpr (requires { operator co_await(static_cast<A &&>(a)); })
        return operator co_await(static_cast<A &&>(a));
    else
        return static_cast<A &&>(a);
}

template<typename A>
using awaiter_t = decltype(get_awaiter(std::declval<A>()));

} // namespace detail

/// An awaitable that applies a synchronous `fn` to another awaitable's
/// result, without creating a coroutine frame.
///
/// Readiness and suspension are forwarded unchanged and `fn` runs inside
/// `await_resume`, so a ready source (a ready `hope`) stays ready and a
/// suspending source (a task or wish) costs only its own suspension. Works
/// over any awaitable: wishes, `hope`, tasks. If the source throws, `fn` is
/// not called.
///
/// The result can only be `co_await`ed, once, as an rvalue; it is not a
/// `task` and cannot be stored in a group. Use `then` for that. Make one
/// with `map(awaitable, fn)` or `awaitable | map(fn)` (the pipe accepts only
/// a `task` on the left).
template<typename Awaitable, typename F>
class mapped
{
public:
    mapped(Awaitable source, F fn)
        : source_(std::move(source))
        , fn_(std::move(fn))
    {}

    auto operator co_await() &&
    {
        struct awaiter
        {
            detail::awaiter_t<Awaitable> inner;
            F fn;

            [[nodiscard]] bool await_ready()
            {
                return inner.await_ready();
            }

            decltype(auto) await_suspend(std::coroutine_handle<> awaiting)
            {
                return inner.await_suspend(awaiting);
            }

            decltype(auto) await_resume()
            {
                if constexpr (std::is_void_v<
                                  decltype(inner.await_resume())>) {
                    inner.await_resume();
                    return std::invoke(std::move(fn));
                } else {
                    return std::invoke(std::move(fn), inner.await_resume());
                }
            }
        };

        return awaiter{
            detail::get_awaiter(std::move(source_)),
            std::move(fn_),
        };
    }

private:
    Awaitable source_;
    F fn_;
};

/// Map an awaitable's result through a synchronous `fn`; see
/// @ref nxtrt::mapped "mapped".
///
/// @code
/// auto doubled = co_await nxtrt::map(
///     nxtrt::hope<int>::ready(21), [](int x) { return x * 2; });
/// @endcode
template<typename Awaitable, typename F>
[[nodiscard]] auto map(Awaitable awaitable, F fn)
{
    return mapped<Awaitable, F>{std::move(awaitable), std::move(fn)};
}

template<typename F>
class map_closure
{
public:
    explicit map_closure(F fn)
        : fn_(std::move(fn))
    {}

    template<typename Awaitable>
    [[nodiscard]] auto operator()(Awaitable && awaitable) &&
    {
        return map(
            std::forward<Awaitable>(awaitable), std::move(fn_));
    }

private:
    F fn_;
};

/// Pipe form: `make_task() | map(f)` gives a @ref nxtrt::mapped "mapped"
/// awaitable over the task.
template<typename F>
[[nodiscard]] auto map(F fn)
{
    return map_closure<std::decay_t<F>>{std::move(fn)};
}

/// A task that awaits `child`, then awaits and returns the task made by
/// `fn(value)` (or `fn()` for a void child).
///
/// This is the asynchronous form of `then`: `fn` returns a `task`, which is
/// created only after `child` succeeds. If `child` throws, `fn` is not
/// called. Stop requests reach whichever of the two is running. Also
/// available as a pipe: `child | let_value(fn)`.
template<typename T, typename F>
[[nodiscard]] task<detail::let_value_result_t<T, F>>
let_value(task<T> child, F fn)
{
    using result_type = detail::let_value_result_t<T, F>;

    if constexpr (std::is_void_v<T>) {
        co_await child;
        auto next = std::invoke(fn);
        if constexpr (std::is_void_v<result_type>) {
            co_await next;
        } else {
            co_return co_await next;
        }
    } else {
        auto value = co_await child;
        auto next = std::invoke(fn, std::move(value));
        if constexpr (std::is_void_v<result_type>) {
            co_await next;
        } else {
            co_return co_await next;
        }
    }
}

/// A task that awaits `child`, then always runs the cleanup task made by
/// `cleanup()`, and finally returns or rethrows `child`'s result.
///
/// The cleanup task is created after `child` settles (success or failure)
/// and runs shielded: stopping the `finally` task does not stop or cancel
/// the cleanup, so it runs to completion. Errors:
/// - only `child` failed: its exception is rethrown after cleanup;
/// - only cleanup failed: the cleanup exception is thrown, and `child`'s
///   value is discarded;
/// - both failed: an `exception_group` holding both (body first) is thrown.
///
/// Also available as a pipe: `child | finally(cleanup)`.
///
/// @code
/// // use_session() returns task<int>; end_session() returns task<void>.
/// int n = co_await nxtrt::finally(
///     use_session(session), [&] { return end_session(session); });
/// @endcode
template<typename T, typename Cleanup>
    requires stored_task_factory<Cleanup>
        && std::is_void_v<stored_task_result_t<Cleanup>>
[[nodiscard]] task<T> finally(task<T> child, Cleanup cleanup)
{
    using result_type = std::conditional_t<
        std::is_void_v<T>,
        std::monostate,
        std::remove_cv_t<T>>;

    auto result = std::optional<result_type>{};
    auto body_failure = std::exception_ptr{};
    auto cleanup_failure = std::exception_ptr{};

    try {
        if constexpr (std::is_void_v<T>) {
            co_await child;
        } else {
            result.emplace(co_await child);
        }
    } catch (...) {
        body_failure = std::current_exception();
    }

    try {
        co_await shield(std::invoke(cleanup));
    } catch (...) {
        cleanup_failure = std::current_exception();
    }

    if (body_failure && cleanup_failure)
        throw_exceptions(
            "task body and cleanup failed",
            {body_failure, cleanup_failure});
    if (cleanup_failure)
        rethrow(std::move(cleanup_failure));
    if (body_failure)
        rethrow(std::move(body_failure));

    if constexpr (!std::is_void_v<T>)
        co_return std::move(*result);
}

template<typename F>
class then_closure
{
public:
    explicit then_closure(F fn)
        : fn_(std::move(fn))
    {}

    template<typename T>
    [[nodiscard]] auto operator()(task<T> child) &&
    {
        return then(std::move(child), std::move(fn_));
    }

private:
    F fn_;
};

template<typename Cleanup>
class finally_closure
{
public:
    explicit finally_closure(Cleanup cleanup)
        : cleanup_(std::move(cleanup))
    {}

    template<typename T>
    [[nodiscard]] auto operator()(task<T> child) &&
    {
        return finally(std::move(child), std::move(cleanup_));
    }

private:
    Cleanup cleanup_;
};

/// Pipe form of `then`: `child | then(fn)`.
template<typename F>
[[nodiscard]] auto then(F fn)
{
    return then_closure<std::decay_t<F>>{std::forward<F>(fn)};
}

template<typename F>
class let_value_closure
{
public:
    explicit let_value_closure(F fn)
        : fn_(std::move(fn))
    {}

    template<typename T>
    [[nodiscard]] auto operator()(task<T> child) &&
    {
        return let_value(std::move(child), std::move(fn_));
    }

private:
    F fn_;
};

/// Pipe form of `let_value`: `child | let_value(fn)`.
template<typename F>
[[nodiscard]] auto let_value(F fn)
{
    return let_value_closure<std::decay_t<F>>{std::forward<F>(fn)};
}

/// Pipe form of `finally`: `child | finally(cleanup)`.
template<typename Cleanup>
[[nodiscard]] auto finally(Cleanup cleanup)
{
    return finally_closure<std::decay_t<Cleanup>>{
        std::forward<Cleanup>(cleanup)};
}

/// Apply a pipe adaptor to a task: `child | adaptor` is
/// `std::move(adaptor)(std::move(child))`. Adaptors come from the one-argument
/// forms of `then`, `let_value`, `finally` and `map`, and chain left to
/// right. `map` can only come last, since it yields an awaitable rather than
/// a task.
///
/// @code
/// auto n = co_await (
///     fetch_count()
///     | nxtrt::then([](int n) { return n * 2; })
///     | nxtrt::finally([&] { return log_done(); }));
/// @endcode
template<typename T, typename Adaptor>
    requires requires(task<T> child, Adaptor adaptor) {
        std::move(adaptor)(std::move(child));
    }
[[nodiscard]] auto operator|(task<T> child, Adaptor adaptor)
{
    return std::move(adaptor)(std::move(child));
}

} // namespace nxtrt
