#pragma once

#include "nxtrt/debug.hpp"
#include "nxtrt/deck.hpp"
#include "nxtrt/env.hpp"
#include "nxtrt/exceptions.hpp"
#include "nxtrt/ids.hpp"
#include "nxtrt/land.hpp"
#include "nxtrt/alloc_trace.hpp"
#include "nxtrt/trace.hpp"
#include "nxtrt/wand.hpp"
#include "nxtrt/wish.hpp"
#include "nxtrt/wish_ops.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <type_traits>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// Coroutine promises, completion hooks, and stop propagation.
// Include nxtrt/task.hpp for the complete runtime API.


namespace nxtrt {

template<typename T>
class deed;

template<typename T>
class catching_deed;

template<typename T>
class deed_result_storage;

namespace detail {

struct promise_base;
struct child_record_base;
struct deed_result_state_base;

/// A stable, non-owning observer of final suspension. Notification must not
/// destroy the completing frame: final_suspend still has work to do.
struct completion_observer
{
    virtual void task_completed() noexcept = 0;
protected:
    ~completion_observer() = default;
};

struct parent_stop_callback_fn
{
    promise_base * child = nullptr;
    void operator()() const noexcept;
};

struct wait_stop_callback_fn
{
    wand * owner = nullptr;
    coin_t token;
    void operator()() const noexcept;
};

/// Shared promise state for every `task<T>`.
///
/// When a function returns `task<T>` and contains `co_await` or `co_return`,
/// the compiler lowers it into a coroutine frame. That frame contains a
/// `promise<T>` object. The promise is the runtime's control block: it creates
/// the public `task<T>` object, receives returned values/exceptions, and decides
/// what happens at initial and final suspension points.
struct promise_base
{
    using parent_stop_callback_type =
        std::stop_callback<parent_stop_callback_fn>;
    using wait_stop_callback_type =
        std::stop_callback<wait_stop_callback_fn>;

    /// Awaited by the compiler when the coroutine body reaches its end.
    ///
    /// We use final suspend to enqueue the awaiting continuation instead
    /// of resuming it inline. That preserves the deck rule: coroutines run
    /// when the pump resumes a ready handle.
    struct final_awaitable
    {
        /// `false` means the coroutine always suspends at final suspend.
        ///
        /// The frame must remain alive after completion so the `task<T>` owner
        /// can read the result or exception from the promise.
        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        /// Called by the compiler after the coroutine has stored its result.
        template<typename Promise>
        void await_suspend(std::coroutine_handle<Promise> coroutine) noexcept
        {
            coroutine.promise().run_completion_callback();
            coroutine.promise().resume_continuation();
        }

        void await_resume() const noexcept {}
    };

    /// Capture the ambient environment visible at coroutine frame creation.
    promise_base()
    {
        if (auto * current = detail::current_env)
            env.copy_entries_from(*current);
    }

    /// Called by the compiler before running the coroutine body.
    ///
    /// `suspend_always` makes tasks lazy: constructing a `task<T>` only creates
    /// the coroutine frame. The deck starts it later by enqueueing the
    /// handle.
    [[nodiscard]] auto initial_suspend() noexcept
    {
        return std::suspend_always{};
    }

    /// Called by the compiler after normal return or unhandled exception.
    [[nodiscard]] auto final_suspend() noexcept
    {
        return final_awaitable{};
    }

    /// Remember the coroutine that should continue after this task completes.
    void set_continuation(
        std::coroutine_handle<> handle,
        promise_base * promise) noexcept
    {
        continuation = handle;
        continuation_promise = promise;
    }

    void follow_stop(promise_base & parent)
    {
        parent_stop_callback.reset();
        auto token = parent.stop_token();
        if (!token.stop_possible())
            return;

        parent_stop_callback.emplace(
            token,
            parent_stop_callback_fn{this});
    }

    void cancel_wait_on_stop(wand & w, coin_t token)
    {
        wait_stop_callback.reset();
        auto stop = stop_token();
        if (!stop.stop_possible())
            return;

        wait_stop_callback.emplace(
            stop,
            wait_stop_callback_fn{&w, token});
    }

    void clear_wait_stop_callback() noexcept
    {
        wait_stop_callback.reset();
    }

    bool request_stop() noexcept
    {
        return stop_.request_stop();
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stop_.stop_requested();
    }

    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        return stop_.get_token();
    }

    void resume_continuation() noexcept
    {
        auto * current = detail::current_env;
        if (continuation
            && current != nullptr
            && current->current_deck != nullptr)
            current->current_deck->enqueue(continuation, continuation_promise);
    }

    void run_completion_callback() noexcept
    {
        if (completion != nullptr)
            completion->task_completed();
    }

    void observe_completion_of(completion_observer & observer) noexcept
    {
        completion = &observer;
    }

    void enqueue_self(std::coroutine_handle<> handle)
    {
        auto * current = detail::current_env;
        if (current == nullptr || current->current_deck == nullptr)
            throw runtime_error{"nxtrt task enqueued without a deck"};
        current->current_deck->enqueue(handle, this);
    }

    void unregister_from_deck() noexcept;

    /// Identity assigned by the deck task registry when the task is first
    /// scheduled.
    task_id id;
    /// Deck registry that currently owns this task id, if any.
    deck * registered_deck = nullptr;
    /// Raw coroutine handle for the awaiting task.
    std::coroutine_handle<> continuation;
    /// Awaiting task promise, used to restore ambient context when continuation runs.
    promise_base * continuation_promise = nullptr;
    /// Promise-owned ambient environment captured by this coroutine frame.
    runtime_env env;
    /// Propagates stop from the task awaiting this task.
    std::optional<parent_stop_callback_type> parent_stop_callback;
    /// Cancels the current parked wish when this task is stopped.
    std::optional<wait_stop_callback_type> wait_stop_callback;
    /// Optional owner to notify when this task reaches final suspend.
    completion_observer * completion = nullptr;

private:
    std::stop_source stop_;
};

inline void parent_stop_callback_fn::operator()() const noexcept
{
    if (child != nullptr)
        child->request_stop();
}

inline void wait_stop_callback_fn::operator()() const noexcept
{
    if (owner != nullptr)
        owner->cancel(token);
}

/// Promise for non-void task results.
template<typename T>
struct promise final : promise_base
{
    using task_type = task<T>;
    using stored_type = std::remove_cv_t<T>;
    using storage_type =
        std::variant<std::monostate, stored_type, std::exception_ptr>;

    /// Called by the compiler immediately after constructing the promise.
    ///
    /// This wraps the coroutine handle in our movable RAII `task<T>` object.
    [[nodiscard]] task_type get_return_object() noexcept;

    /// Called by the compiler for `co_return value;`.
    template<typename Value>
        requires std::is_constructible_v<stored_type, Value &&>
    void return_value(Value && value)
    {
        storage_.template emplace<stored_type>(
            std::forward<Value>(value));
    }

    /// Called by the compiler if an exception escapes the coroutine body.
    void unhandled_exception() noexcept
    {
        storage_.template emplace<std::exception_ptr>(
            std::current_exception());
    }

    /// Read the completed result, rethrowing any stored exception.
    T & result() &
    {
        if (std::holds_alternative<stored_type>(storage_))
            return std::get<stored_type>(storage_);
        if (std::holds_alternative<std::exception_ptr>(storage_))
            rethrow(std::get<std::exception_ptr>(storage_));
        throw runtime_error{"nxtrt task result was never set"};
    }

    /// Move the completed result out of the promise.
    T && result() &&
    {
        if (std::holds_alternative<stored_type>(storage_))
            return std::move(std::get<stored_type>(storage_));
        if (std::holds_alternative<std::exception_ptr>(storage_))
            rethrow(std::get<std::exception_ptr>(storage_));
        throw runtime_error{"nxtrt task result was never set"};
    }

private:
    storage_type storage_;
};

/// Promise specialization for `task<void>`.
template<>
struct promise<void> final : promise_base
{
    using task_type = task<void>;

    [[nodiscard]] task_type get_return_object() noexcept;

    /// Called by the compiler for bare `co_return;` or falling off the end.
    void return_void() noexcept {}

    void unhandled_exception() noexcept
    {
        exception_ = std::current_exception();
    }

    /// Re-throw any exception captured from the coroutine body.
    void result()
    {
        if (exception_)
            rethrow(exception_);
    }

private:
    std::exception_ptr exception_;
};

} // namespace detail

} // namespace nxtrt
