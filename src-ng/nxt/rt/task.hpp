#pragma once

#include "nxt/rt/deck.hpp"

#include <coroutine>
#include <exception>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace nxt::rt {

namespace detail {

// Global id source for this early experiment. A later runtime object may own
// id allocation if we want per-runtime numbering.
inline task_id_source task_ids;

/// Shared promise state for every `task<T>`.
///
/// When a function returns `task<T>` and contains `co_await` or `co_return`,
/// the compiler lowers it into a coroutine frame. That frame contains a
/// `promise<T>` object. The promise is the runtime's control block: it creates
/// the public `task<T>` object, receives returned values/exceptions, and decides
/// what happens at initial and final suspension points.
struct promise_base
{
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
            coroutine.promise().resume_continuation();
        }

        void await_resume() const noexcept {}
    };

    promise_base() noexcept
        : id(task_ids.next())
    {}

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

    void resume_continuation() noexcept
    {
        if (continuation && detail::current_deck != nullptr)
            detail::current_deck->enqueue(continuation, continuation_promise);
    }

    void enqueue_self(std::coroutine_handle<> handle)
    {
        if (detail::current_deck == nullptr)
            throw std::runtime_error{"nxt::rt task enqueued without a deck"};
        detail::current_deck->enqueue(handle, this);
    }

    /// Identity assigned when the coroutine frame is created.
    task_id id;
    /// Raw coroutine handle for the awaiting task.
    std::coroutine_handle<> continuation;
    /// Awaiting task promise, used to restore ambient context when continuation runs.
    promise_base * continuation_promise = nullptr;
};

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
            std::rethrow_exception(std::get<std::exception_ptr>(storage_));
        throw std::runtime_error{"nxt::rt task result was never set"};
    }

    /// Move the completed result out of the promise.
    T && result() &&
    {
        if (std::holds_alternative<stored_type>(storage_))
            return std::move(std::get<stored_type>(storage_));
        if (std::holds_alternative<std::exception_ptr>(storage_))
            std::rethrow_exception(std::get<std::exception_ptr>(storage_));
        throw std::runtime_error{"nxt::rt task result was never set"};
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
            std::rethrow_exception(exception_);
    }

private:
    std::exception_ptr exception_;
};

} // namespace detail

template<typename T>
class [[nodiscard]] task
{
public:
    /// Name the promise type so the compiler knows which promise to put in the
    /// coroutine frame for functions returning `task<T>`.
    using promise_type = detail::promise<T>;
    using coroutine_handle = std::coroutine_handle<promise_type>;

    /// Awaiter used when another coroutine does `co_await some_task`.
    ///
    /// Awaiting wires child -> continuation: the awaiting task becomes the
    /// child's continuation, and the child is enqueued on the same deck.
    class awaiter
    {
    public:
        explicit awaiter(coroutine_handle coroutine) noexcept
            : coroutine_(coroutine)
        {}

        /// If the child already completed, the awaiting coroutine need not
        /// suspend; `await_resume()` can immediately read the result.
        [[nodiscard]] bool await_ready() const noexcept
        {
            return !coroutine_ || coroutine_.done();
        }

        /// Called by the compiler when the awaiting coroutine suspends.
        ///
        /// `awaiting` is the awaiting coroutine handle. We store it as the
        /// child's continuation and enqueue the child for the pump.
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            auto * active_deck = detail::current_deck;
            auto * awaiting_promise = detail::current_promise;
            if (active_deck == nullptr || awaiting_promise == nullptr)
                throw std::runtime_error{
                    "nxt::rt task awaited without a running deck"};

            auto & promise = coroutine_.promise();
            promise.set_continuation(awaiting, awaiting_promise);
            active_deck->enqueue(coroutine_, &promise);
        }

        /// Called when the awaiting task resumes after the child reaches final suspend.
        decltype(auto) await_resume()
        {
            return coroutine_.promise().result();
        }

    private:
        coroutine_handle coroutine_;
    };

    task() noexcept = default;

    /// Constructed by `promise<T>::get_return_object()`.
    explicit task(coroutine_handle coroutine) noexcept
        : coroutine_(coroutine)
    {}

    /// Tasks uniquely own their coroutine frame.
    task(const task &) = delete;
    task & operator=(const task &) = delete;

    /// Moving transfers frame ownership; the moved-from task becomes empty.
    task(task && other) noexcept
        : coroutine_(std::exchange(other.coroutine_, nullptr))
    {}

    task & operator=(task && other) noexcept
    {
        if (this != &other) {
            destroy();
            coroutine_ = std::exchange(other.coroutine_, nullptr);
        }
        return *this;
    }

    /// Destroying a task destroys its coroutine frame if it still owns one.
    ~task()
    {
        destroy();
    }

    /// True for an empty task or a coroutine that has reached final suspend.
    [[nodiscard]] bool done() const noexcept
    {
        return !coroutine_ || coroutine_.done();
    }

    /// The id assigned to this task's promise, or empty for a moved-from task.
    [[nodiscard]] task_id id() const noexcept
    {
        if (!coroutine_)
            return {};
        return coroutine_.promise().id;
    }

    /// Raw coroutine handle. Low-level deck plumbing only.
    [[nodiscard]] coroutine_handle handle() const noexcept
    {
        return coroutine_;
    }

    /// Coroutine customization point for `co_await task`.
    [[nodiscard]] auto operator co_await() & noexcept
    {
        return awaiter{coroutine_};
    }

    /// Rvalue overload so `co_await make_task()` also works.
    [[nodiscard]] auto operator co_await() && noexcept
    {
        return awaiter{coroutine_};
    }

    /// Read the result from an already completed task.
    decltype(auto) result() &
    {
        return coroutine_.promise().result();
    }

    /// Move the result from an already completed task.
    decltype(auto) result() &&
    {
        return std::move(coroutine_.promise()).result();
    }

private:
    bool destroy() noexcept
    {
        if (!coroutine_)
            return false;
        coroutine_.destroy();
        coroutine_ = nullptr;
        return true;
    }

    coroutine_handle coroutine_{nullptr};
};

namespace detail {

template<typename T>
inline auto promise<T>::get_return_object() noexcept -> task_type
{
    return task_type{
        std::coroutine_handle<promise<T>>::from_promise(*this)};
}

inline auto promise<void>::get_return_object() noexcept -> task_type
{
    return task_type{
        std::coroutine_handle<promise<void>>::from_promise(*this)};
}

} // namespace detail

inline task_id deck::current_task_id() const noexcept
{
    if (detail::current_deck != this || detail::current_promise == nullptr)
        return {};
    return detail::current_promise->id;
}

inline void deck::enqueue(
    std::coroutine_handle<> handle,
    detail::promise_base * promise)
{
    trace("deck enqueue task");
    ready_.push_back(
        deck::ready_item{
            .handle = handle,
            .promise = promise,
        });
}

inline void deck::ready_item::resume_if_ready() const
{
    if (!handle || handle.done())
        return;

    trace("deck resume task");
    auto promise_guard = current_promise_guard{promise};
    handle.resume();
}

inline void parked_task::resume(deck & d) const
{
    trace("wand fulfill parked task");
    d.enqueue(handle, promise);
}

template<typename T>
inline void waiter<T>::await_suspend(
    std::coroutine_handle<> awaiting) const
{
    auto * active_wand = source_;
    auto * running = detail::current_promise;
    if (active_wand == nullptr || running == nullptr)
        throw std::runtime_error{
            "nxt::rt waiter awaited without a prepared wand"};

    trace("waiter suspend token=" + std::to_string(token_));
    active_wand->suspend(
        token_,
        parked_task{
            .handle = awaiting,
            .promise = running,
        });
}

inline waiter<void> manual_wish::operator co_await() const
{
    auto * active_deck = detail::current_deck;
    auto * active_wand = detail::current_wand;
    auto * running = detail::current_promise;
    if (active_deck == nullptr || active_wand == nullptr || running == nullptr)
        throw std::runtime_error{
            "nxt::rt manual wish awaited without a running wand"};

    trace("wish manual prepare token=" + std::to_string(token));
    return active_wand->prepare(*active_deck, *running, *this);
}

inline waiter<int> openat_wish::operator co_await() const
{
    auto * active_deck = detail::current_deck;
    auto * active_wand = detail::current_wand;
    auto * running = detail::current_promise;
    if (active_deck == nullptr || active_wand == nullptr || running == nullptr)
        throw std::runtime_error{
            "nxt::rt openat wish awaited without a running wand"};

    trace("wish openat prepare path=" + path);
    return active_wand->prepare(*active_deck, *running, *this);
}

inline waiter<std::size_t> read_some_wish::operator co_await() const
{
    auto * active_deck = detail::current_deck;
    auto * active_wand = detail::current_wand;
    auto * running = detail::current_promise;
    if (active_deck == nullptr || active_wand == nullptr || running == nullptr)
        throw std::runtime_error{
            "nxt::rt read wish awaited without a running wand"};

    trace("wish read prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return active_wand->prepare(*active_deck, *running, *this);
}

inline waiter<std::size_t> recv_some_wish::operator co_await() const
{
    auto * active_deck = detail::current_deck;
    auto * active_wand = detail::current_wand;
    auto * running = detail::current_promise;
    if (active_deck == nullptr || active_wand == nullptr || running == nullptr)
        throw std::runtime_error{
            "nxt::rt recv wish awaited without a running wand"};

    trace("wish recv prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return active_wand->prepare(*active_deck, *running, *this);
}

inline waiter<std::size_t> send_some_wish::operator co_await() const
{
    auto * active_deck = detail::current_deck;
    auto * active_wand = detail::current_wand;
    auto * running = detail::current_promise;
    if (active_deck == nullptr || active_wand == nullptr || running == nullptr)
        throw std::runtime_error{
            "nxt::rt send wish awaited without a running wand"};

    trace("wish send prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return active_wand->prepare(*active_deck, *running, *this);
}

struct yield_awaiter
{
    /// Yielding always suspends so the coroutine returns to the pump.
    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    /// Re-enqueue the currently running coroutine.
    void await_suspend(std::coroutine_handle<> awaiting) const
    {
        auto * active_deck = detail::current_deck;
        auto * running = detail::current_promise;
        if (active_deck == nullptr || running == nullptr)
            throw std::runtime_error{
                "nxt::rt yield awaited without a running deck"};
        active_deck->enqueue(awaiting, running);
    }

    /// No value is produced by `co_await nxt::rt::yield()`.
    void await_resume() const noexcept {}
};

inline yield_awaiter yield() noexcept
{
    return yield_awaiter{};
}

template<typename T>
inline void deck::start(task<T> & t)
{
    auto handle = t.handle();
    if (!handle || handle.done())
        return;
    enqueue(handle, &handle.promise());
}

} // namespace nxt::rt
