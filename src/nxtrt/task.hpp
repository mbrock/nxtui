#pragma once

#include "nxtrt/debug.hpp"
#include "nxtrt/deck.hpp"
#include "nxtrt/env.hpp"
#include "nxtrt/exceptions.hpp"
#include "nxtrt/ids.hpp"
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

namespace nxtrt {

template<typename T>
class deed;

template<typename T>
class catching_deed;

template<typename T>
class deed_result_storage;

namespace detail {

void * allocate_task_frame(std::size_t size);
void deallocate_task_frame(void * ptr, std::size_t size) noexcept;
struct promise_base;
struct child_record_base;
struct deed_result_state_base;
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

    [[nodiscard]] static void * operator new(std::size_t size)
    {
        return allocate_task_frame(size);
    }

    static void operator delete(void * ptr, std::size_t size) noexcept
    {
        deallocate_task_frame(ptr, size);
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

    template<typename Yielded>
    decltype(auto) yield_value(Yielded && yielded);

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

    void run_completion_callback() noexcept;

    void observe_completion_of(child_record_base & child) noexcept
    {
        completion_child = &child;
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
    /// Optional firm child record to notify when this task reaches final suspend.
    child_record_base * completion_child = nullptr;

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

template<typename T>
class deed_result_storage
{
public:
    using stored_type = std::remove_cv_t<T>;

    deed_result_storage() = default;
    deed_result_storage(const deed_result_storage &) = delete;
    deed_result_storage & operator=(const deed_result_storage &) = delete;
    deed_result_storage(deed_result_storage &&) = delete;
    deed_result_storage & operator=(deed_result_storage &&) = delete;

    [[nodiscard]] bool ready() const noexcept
    {
        return value_.has_value();
    }

    template<typename Value>
        requires std::constructible_from<stored_type, Value &&>
    void set_value(Value && value)
    {
        value_.emplace(std::forward<Value>(value));
    }

    [[nodiscard]] T take_result()
    {
        if (!value_)
            throw runtime_error{"nxtrt task result was never set"};
        auto result = std::move(*value_);
        value_.reset();
        return result;
    }

private:
    std::optional<stored_type> value_;
};

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
        explicit awaiter(
            coroutine_handle coroutine,
            bool follow_parent_stop = true) noexcept
            : coroutine_(coroutine)
            , follow_parent_stop_(follow_parent_stop)
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
            task::splice_handle(coroutine_, awaiting, follow_parent_stop_);
        }

        /// Called when the awaiting task resumes after the child reaches final suspend.
        decltype(auto) await_resume()
        {
            if constexpr (std::is_void_v<T>) {
                return coroutine_.promise().result();
            } else {
                return std::move(coroutine_.promise()).result();
            }
        }

    private:
        coroutine_handle coroutine_;
        bool follow_parent_stop_ = true;
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

    bool request_stop() noexcept
    {
        if (!coroutine_)
            return false;
        return coroutine_.promise().request_stop();
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return coroutine_ && coroutine_.promise().stop_requested();
    }

    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        if (!coroutine_)
            return {};
        return coroutine_.promise().stop_token();
    }

    /// Transfer frame ownership to low-level runtime machinery.
    [[nodiscard]] coroutine_handle release() noexcept
    {
        return std::exchange(coroutine_, nullptr);
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

    /// Schedule this task on the current deck so that `awaiting` resumes,
    /// with this task's result available, once this task completes. The
    /// continuation wiring (ambient env snapshot and stop propagation) is the
    /// same as the one `co_await` performs.
    ///
    /// This is the splice primitive for custom awaitables. When a synchronous
    /// fast path in `await_ready()` misses, store a delegate task in the
    /// awaitable and call `splice_onto(awaiting)` from `await_suspend()`; the
    /// delegate then resumes `awaiting` when it finishes. The delegate frame
    /// must outlive the suspension, so the awaitable must own the task rather
    /// than splice a temporary -- hence the lvalue-ref qualifier.
    void splice_onto(
        std::coroutine_handle<> awaiting,
        bool follow_stop = true) &
    {
        if (!coroutine_)
            throw runtime_error{"nxtrt splice_onto on an empty task"};
        splice_handle(coroutine_, awaiting, follow_stop);
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
    /// Shared continuation-splice used by both `co_await` (via `awaiter`) and
    /// the public `splice_onto`. Wires `awaiting` as `child`'s continuation,
    /// copies the ambient env into the child, optionally follows the awaiting
    /// task's stop, and enqueues the child on the current deck.
    static void splice_handle(
        coroutine_handle child,
        std::coroutine_handle<> awaiting,
        bool follow_stop)
    {
        auto * current = detail::current_env;
        auto * active_deck =
            current == nullptr ? nullptr : current->current_deck;
        auto * awaiting_promise =
            current == nullptr ? nullptr : current->current_promise;
        if (active_deck == nullptr || awaiting_promise == nullptr)
            throw runtime_error{
                "nxtrt task spliced without a running deck"};

        auto & promise = child.promise();
        promise.env.copy_entries_from(*current);
        promise.set_continuation(awaiting, awaiting_promise);
        if (follow_stop)
            promise.follow_stop(*awaiting_promise);
        active_deck->enqueue(child, &promise);
    }

    bool destroy() noexcept
    {
        if (!coroutine_)
            return false;
        auto & promise = coroutine_.promise();
        debug::unpark_task(promise.id);
        promise.unregister_from_deck();
        coroutine_.destroy();
        coroutine_ = nullptr;
        return true;
    }

    coroutine_handle coroutine_{nullptr};
};

/// A hope is the sum of a synchronous result and a pending coroutine: it is
/// `ready(T)` when the value is already available, or a `task<T>` when it is
/// not. Awaiting a ready hope never suspends (the value is returned inline);
/// awaiting a pending hope splices the task as the awaiter continuation and
/// resumes with its result.
///
/// This is the seam between functional (wish-like) and coroutine (task-like)
/// composition: a reader's `take(n)` is a plain function that returns
/// `hope<T>::ready(span)` on a buffer hit -- no frame, no suspension -- and a
/// `task<T>` that loops over real reads on a miss, allocating a frame only
/// then.
template<typename T>
class hope
{
public:
    /// Pending: a coroutine that produces `T`, run only when awaited.
    hope(task<T> pending, bool follow_stop = true)
        : state_(std::in_place_type<task<T>>, std::move(pending))
        , follow_stop_(follow_stop)
    {}

    /// Ready: the value is already available; awaiting will not suspend.
    static hope ready(T value)
    {
        return hope{ready_tag{}, std::move(value)};
    }

    [[nodiscard]] bool await_ready() const noexcept
    {
        return std::holds_alternative<T>(state_);
    }

    [[nodiscard]] bool is_ready() const noexcept
    {
        return await_ready();
    }

    T take_ready()
    {
        if (auto * value = std::get_if<T>(&state_))
            return std::move(*value);
        throw runtime_error{"nxtrt hope is not ready"};
    }

    void await_suspend(std::coroutine_handle<> awaiting)
    {
        std::get<task<T>>(state_).splice_onto(awaiting, follow_stop_);
    }

    T await_resume()
    {
        if (auto * value = std::get_if<T>(&state_))
            return std::move(*value);
        return std::move(std::get<task<T>>(state_)).result();
    }

private:
    struct ready_tag
    {};

    hope(ready_tag, T value)
        : state_(std::in_place_type<T>, std::move(value))
    {}

    std::variant<T, task<T>> state_;
    bool follow_stop_ = true;
};

template<>
class hope<void>
{
public:
    hope(task<void> pending, bool follow_stop = true)
        : state_(std::in_place_type<task<void>>, std::move(pending))
        , follow_stop_(follow_stop)
    {}

    static hope ready()
    {
        return hope{};
    }

    [[nodiscard]] bool await_ready() const noexcept
    {
        return std::holds_alternative<std::monostate>(state_);
    }

    [[nodiscard]] bool is_ready() const noexcept
    {
        return await_ready();
    }

    void take_ready()
    {
        if (!is_ready())
            throw runtime_error{"nxtrt hope is not ready"};
    }

    void await_suspend(std::coroutine_handle<> awaiting)
    {
        std::get<task<void>>(state_).splice_onto(awaiting, follow_stop_);
    }

    void await_resume()
    {
        if (auto * pending = std::get_if<task<void>>(&state_))
            std::move(*pending).result();
    }

private:
    hope() = default;

    std::variant<std::monostate, task<void>> state_;
    bool follow_stop_ = true;
};

namespace detail {

/// Run `fn` with `Key` temporarily bound in the current task environment.
///
/// The binding mutates the promise-owned environment and restores the previous
/// entry when the scoped child task completes.
template<typename Key, stored_task_factory Fn>
[[nodiscard]] task<stored_task_result_t<Fn>>
with_env_bound(typename Key::value_type value, Fn fn)
{
    auto * current = detail::current_env;
    auto * promise = current == nullptr ? nullptr : current->current_promise;
    if (current == nullptr || promise == nullptr)
        throw runtime_error{
            "nxtrt env binding used without runtime env"};

    struct binding_guard
    {
        detail::promise_base * promise = nullptr;
        runtime_env::entry_snapshot previous;

        ~binding_guard() noexcept
        {
            if (promise != nullptr)
                promise->env.restore(std::move(previous));
        }
    };

    auto restore = binding_guard{
        .promise = promise,
        .previous = promise->env.template replace<Key>(std::move(value)),
    };
    auto child = std::invoke(fn);

    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        co_await child;
    } else {
        co_return co_await child;
    }
}

} // namespace detail

template<typename Key, typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] task<stored_task_result_t<std::decay_t<Fn>>>
with_env(typename Key::value_type value, Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    return detail::with_env_bound<Key>(
        std::move(value),
        factory_type{std::forward<Fn>(fn)});
}

template<task_factory Fn>
[[nodiscard]] task<task_result_t<std::invoke_result_t<Fn>>>
with_trace_span(std::string name, trace_attributes attributes, Fn && fn)
{
    auto context = current_trace_context();
    if (context == nullptr) {
        auto child = std::invoke(std::forward<Fn>(fn));
        if constexpr (std::is_void_v<task_result_t<std::invoke_result_t<Fn>>>) {
            co_await child;
        } else {
            co_return co_await child;
        }
    } else {
        auto span = context->start_span(
            std::move(name), current_trace_span_id(), std::move(attributes));
        try {
            if constexpr (
                std::is_void_v<task_result_t<std::invoke_result_t<Fn>>>) {
                co_await with_env<trace_current_span_key>(
                    span.span_id(), [&]() -> task<void> {
                    co_await std::invoke(fn);
                });
                span.finish("ok");
            } else {
                auto result = co_await with_env<trace_current_span_key>(
                    span.span_id(),
                    [&]() -> task<task_result_t<std::invoke_result_t<Fn>>> {
                    co_return co_await std::invoke(fn);
                });
                span.finish("ok");
                co_return std::move(result);
            }
        } catch (...) {
            span.finish("error");
            throw;
        }
    }
}

template<task_factory Fn>
[[nodiscard]] task<task_result_t<std::invoke_result_t<Fn>>>
with_trace_span(std::string name, Fn && fn)
{
    return with_trace_span(
        std::move(name), {}, std::forward<Fn>(fn));
}

namespace detail {

class started_handle_awaiter
{
public:
    template<typename Promise>
    explicit started_handle_awaiter(
        std::coroutine_handle<Promise> handle) noexcept
        : handle_(handle)
        , promise_(handle ? &handle.promise() : nullptr)
    {}

    [[nodiscard]] bool await_ready() const noexcept
    {
        return !handle_ || handle_.done();
    }

    void await_suspend(std::coroutine_handle<> awaiting) const
    {
        auto * current = detail::current_env;
        auto * awaiting_promise =
            current == nullptr ? nullptr : current->current_promise;
        if (awaiting_promise == nullptr || promise_ == nullptr)
            throw runtime_error{
                "nxtrt firm join used without a running task"};

        promise_->set_continuation(awaiting, awaiting_promise);
    }

    void await_resume() const noexcept {}

private:
    std::coroutine_handle<> handle_;
    promise_base * promise_ = nullptr;
};

struct firm_child_record_header
{
    firm * owner = nullptr;
    task_id task;
    bool completion_reported = false;
};

struct child_completion
{
    task_id child;
    std::exception_ptr failure;
};

struct child_record_base
{
    child_record_base() = default;
    child_record_base(const child_record_base &) = delete;
    child_record_base & operator=(const child_record_base &) = delete;
    child_record_base(child_record_base &&) = delete;
    child_record_base & operator=(child_record_base &&) = delete;
    virtual ~child_record_base() = default;

    [[nodiscard]] virtual bool done() const noexcept = 0;
    [[nodiscard]] virtual bool joined() const noexcept = 0;
    [[nodiscard]] virtual std::exception_ptr completion_failure()
        noexcept = 0;
    [[nodiscard]] virtual std::exception_ptr failure() = 0;
    [[nodiscard]] virtual bool result_contained() const noexcept = 0;
    [[nodiscard]] virtual bool result_observed() const noexcept = 0;
    [[nodiscard]] virtual bool result_exported() const noexcept = 0;
    [[nodiscard]] virtual task<void> join() = 0;
    virtual void evacuate_result_if_done() = 0;
    virtual void drop_result_state(
        deed_result_state_base * result) noexcept = 0;
    virtual void replace_result_state(
        deed_result_state_base * old_result,
        deed_result_state_base * new_result) noexcept = 0;
    virtual void request_stop() noexcept = 0;

    void report_finished_from_promise() noexcept;

    firm_child_record_header firm_record;
};

inline void promise_base::run_completion_callback() noexcept
{
    if (completion_child == nullptr)
        return;
    completion_child->report_finished_from_promise();
}

struct deed_record_header
{
    child_record_base * child = nullptr;
    task_id child_task;
    bool contained = false;
    bool observed = false;
    bool result_taken = false;
};

struct firm_deed_record
{
    task_id child;
};

struct deed_result_state_base
{
    deed_result_state_base() = default;
    deed_result_state_base(const deed_result_state_base &) = delete;
    deed_result_state_base & operator=(
        const deed_result_state_base &) = delete;
    deed_result_state_base(deed_result_state_base && other) noexcept
        : record{
              .child = std::exchange(other.record.child, nullptr),
              .child_task = std::exchange(other.record.child_task, {}),
              .contained = std::exchange(other.record.contained, false),
              .observed = std::exchange(other.record.observed, false),
              .result_taken =
                  std::exchange(other.record.result_taken, false),
          }
    {
        if (record.child != nullptr)
            record.child->replace_result_state(&other, this);
    }

    deed_result_state_base & operator=(
        deed_result_state_base && other) noexcept
    {
        if (this == &other)
            return *this;
        detach();
        record.child = std::exchange(other.record.child, nullptr);
        record.child_task = std::exchange(other.record.child_task, {});
        record.contained =
            std::exchange(other.record.contained, false);
        record.observed = std::exchange(other.record.observed, false);
        record.result_taken =
            std::exchange(other.record.result_taken, false);
        if (record.child != nullptr)
            record.child->replace_result_state(&other, this);
        return *this;
    }

    virtual ~deed_result_state_base()
    {
        detach();
    }

    void detach() noexcept
    {
        if (record.child == nullptr)
            return;
        auto * old_child = record.child;
        record.child = nullptr;
        old_child->drop_result_state(this);
    }

    void ensure_ready_from_child()
    {
        if (record.child != nullptr && record.child->done())
            record.child->evacuate_result_if_done();
    }

    deed_record_header record;
};

template<typename T>
struct deed_result_slot
{
    using stored_type = std::remove_cv_t<T>;
    using storage_type =
        std::variant<
            std::monostate,
            stored_type,
            stored_type *,
            deed_result_storage<stored_type> *,
            std::exception_ptr>;

    [[nodiscard]] bool ready() const noexcept
    {
        if (std::holds_alternative<stored_type *>(storage))
            return target_ready;
        if (auto * target = target_cell())
            return target->ready();
        return !std::holds_alternative<std::monostate>(storage);
    }

    template<typename Value>
    void set_value(Value && value)
    {
        if (auto * target = target_storage()) {
            if constexpr (std::assignable_from<stored_type &, Value>) {
                *target = std::forward<Value>(value);
                target_ready = true;
                return;
            } else {
                throw runtime_error{
                    "nxtrt deed result target is not assignable"};
            }
        }
        if (auto * target = target_cell()) {
            if constexpr (std::constructible_from<
                              stored_type,
                              Value &&>) {
                target->set_value(std::forward<Value>(value));
                return;
            } else {
                throw runtime_error{
                    "nxtrt deed result target is not constructible"};
            }
        }
        storage.template emplace<stored_type>(
            std::forward<Value>(value));
    }

    void set_exception(std::exception_ptr failure)
    {
        storage.template emplace<std::exception_ptr>(
            std::move(failure));
        target_ready = false;
    }

    [[nodiscard]] std::exception_ptr failure() const
    {
        if (std::holds_alternative<std::exception_ptr>(storage))
            return std::get<std::exception_ptr>(storage);
        return {};
    }

    [[nodiscard]] T take_result()
    {
        if (std::holds_alternative<std::exception_ptr>(storage))
            rethrow(std::get<std::exception_ptr>(storage));
        if (auto * target = target_storage()) {
            if (!target_ready)
                throw runtime_error{"nxtrt task result was never set"};
            return std::move(*target);
        }
        if (auto * target = target_cell())
            return target->take_result();
        if (!std::holds_alternative<stored_type>(storage))
            throw runtime_error{"nxtrt task result was never set"};
        return std::move(std::get<stored_type>(storage));
    }

    void store_in(stored_type & target)
    {
        if (std::holds_alternative<std::exception_ptr>(storage))
            throw runtime_error{
                "nxtrt deed result target set after failure"};
        if (auto * old_target = target_storage()) {
            if (target_ready)
                throw runtime_error{
                    "nxtrt deed result target set after result ready"};
            if (old_target == &target)
                return;
        } else if (auto * old_target = target_cell()) {
            if (old_target->ready())
                throw runtime_error{
                    "nxtrt deed result target set after result ready"};
        } else if (std::holds_alternative<stored_type>(storage)) {
            target = std::move(std::get<stored_type>(storage));
            target_ready = true;
        }
        storage.template emplace<stored_type *>(&target);
    }

    void store_in(deed_result_storage<stored_type> & target)
    {
        if (std::holds_alternative<std::exception_ptr>(storage))
            throw runtime_error{
                "nxtrt deed result target set after failure"};
        if (target.ready())
            throw runtime_error{
                "nxtrt deed result target set after result ready"};
        if (auto * old_target = target_storage()) {
            if (target_ready)
                throw runtime_error{
                    "nxtrt deed result target set after result ready"};
            static_cast<void>(old_target);
        } else if (auto * old_target = target_cell()) {
            if (old_target->ready())
                throw runtime_error{
                    "nxtrt deed result target set after result ready"};
            if (old_target == &target)
                return;
        } else if (std::holds_alternative<stored_type>(storage)) {
            target.set_value(std::move(std::get<stored_type>(storage)));
        }
        storage.template emplace<deed_result_storage<stored_type> *>(
            &target);
        target_ready = false;
    }

    [[nodiscard]] stored_type * target_storage() noexcept
    {
        if (std::holds_alternative<stored_type *>(storage))
            return std::get<stored_type *>(storage);
        return nullptr;
    }

    [[nodiscard]] const stored_type * target_storage() const noexcept
    {
        if (std::holds_alternative<stored_type *>(storage))
            return std::get<stored_type *>(storage);
        return nullptr;
    }

    [[nodiscard]] deed_result_storage<stored_type> *
    target_cell() noexcept
    {
        if (std::holds_alternative<deed_result_storage<stored_type> *>(
                storage))
            return std::get<deed_result_storage<stored_type> *>(storage);
        return nullptr;
    }

    [[nodiscard]] const deed_result_storage<stored_type> *
    target_cell() const noexcept
    {
        if (std::holds_alternative<deed_result_storage<stored_type> *>(
                storage))
            return std::get<deed_result_storage<stored_type> *>(storage);
        return nullptr;
    }

    storage_type storage;
    bool target_ready = false;
};

template<>
struct deed_result_slot<void>
{
    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }

    void set_value() noexcept
    {
        ready_ = true;
    }

    void set_exception(std::exception_ptr failure)
    {
        failure_ = std::move(failure);
        ready_ = true;
    }

    [[nodiscard]] std::exception_ptr failure() const
    {
        return failure_;
    }

    void take_result()
    {
        if (failure_)
            rethrow(failure_);
    }

    std::exception_ptr failure_;
    bool ready_ = false;
};

template<typename T>
struct deed_result_state final : deed_result_state_base
{
    using stored_type = typename deed_result_slot<T>::stored_type;

    deed_result_state() = default;
    deed_result_state(deed_result_state &&) = default;
    deed_result_state & operator=(deed_result_state &&) = default;

    [[nodiscard]] bool ready() const noexcept
    {
        return slot.ready();
    }

    void ensure_done()
    {
        if (!ready())
            ensure_ready_from_child();
        if (!ready())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    template<typename Value>
    void set_value(Value && value)
    {
        slot.set_value(std::forward<Value>(value));
    }

    void set_exception(std::exception_ptr failure)
    {
        slot.set_exception(std::move(failure));
    }

    [[nodiscard]] std::exception_ptr failure()
    {
        ensure_done();
        return slot.failure();
    }

    [[nodiscard]] std::exception_ptr observe_exception()
    {
        record.observed = true;
        return failure();
    }

    [[nodiscard]] T take_result()
    {
        ensure_done();
        if (record.result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        record.observed = true;
        record.result_taken = true;
        return slot.take_result();
    }

    void store_result_in(stored_type & target)
    {
        if (record.result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        if (!ready())
            ensure_ready_from_child();
        slot.store_in(target);
    }

    void store_result_in(deed_result_storage<stored_type> & target)
    {
        if (record.result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        if (!ready())
            ensure_ready_from_child();
        slot.store_in(target);
    }

    deed_result_slot<T> slot;
};

template<>
struct deed_result_state<void> final : deed_result_state_base
{
    deed_result_state() = default;
    deed_result_state(deed_result_state &&) noexcept = default;
    deed_result_state & operator=(deed_result_state &&) noexcept = default;

    [[nodiscard]] bool ready() const noexcept
    {
        return slot.ready();
    }

    void ensure_done()
    {
        if (!ready())
            ensure_ready_from_child();
        if (!ready())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    void set_value() noexcept
    {
        slot.set_value();
    }

    void set_exception(std::exception_ptr failure)
    {
        slot.set_exception(std::move(failure));
    }

    [[nodiscard]] std::exception_ptr failure()
    {
        ensure_done();
        return slot.failure();
    }

    [[nodiscard]] std::exception_ptr observe_exception()
    {
        record.observed = true;
        return failure();
    }

    void take_result()
    {
        ensure_done();
        if (record.result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        record.observed = true;
        record.result_taken = true;
        slot.take_result();
    }

    deed_result_slot<void> slot;
};

template<typename T>
struct child_record final : child_record_base
{
    using handle_type = typename task<T>::coroutine_handle;

    child_record(
        handle_type h,
        firm & owner,
        deed_result_state<T> * result)
        : handle(h)
        , result(result)
    {
        this->firm_record.owner = &owner;
        if (this->result != nullptr)
            this->result->record.child = this;
    }

    ~child_record() override
    {
        if (result != nullptr)
            result->record.child = nullptr;
        destroy_frame();
    }

    [[nodiscard]] bool done() const noexcept override
    {
        return evacuated_ || !handle || handle.done();
    }

    [[nodiscard]] bool joined() const noexcept override
    {
        return joined_;
    }

    [[nodiscard]] task<void> join() override
    {
        if (!joined_ && handle && !handle.done())
            co_await started_handle_awaiter{handle};
        joined_ = true;
        evacuate_result();
    }

    [[nodiscard]] std::exception_ptr completion_failure()
        noexcept override
    {
        if (!handle || !handle.done())
            return {};
        try {
            static_cast<void>(handle.promise().result());
        } catch (...) {
            return std::current_exception();
        }
        return {};
    }

    [[nodiscard]] std::exception_ptr failure() override
    {
        evacuate_result();
        if (result != nullptr)
            return result->failure();
        return failure_;
    }

    [[nodiscard]] bool result_contained() const noexcept override
    {
        return result != nullptr && result->record.contained;
    }

    [[nodiscard]] bool result_observed() const noexcept override
    {
        return result != nullptr && result->record.observed;
    }

    [[nodiscard]] bool result_exported() const noexcept override
    {
        return result != nullptr;
    }

    void evacuate_result_if_done() override
    {
        if (done())
            evacuate_result();
    }

    void drop_result_state(deed_result_state_base * state) noexcept override
    {
        if (state == result)
            result = nullptr;
    }

    void replace_result_state(
        deed_result_state_base * old_state,
        deed_result_state_base * new_state) noexcept override
    {
        if (old_state == result)
            result = static_cast<deed_result_state<T> *>(new_state);
    }

    void request_stop() noexcept override
    {
        if (handle)
            handle.promise().request_stop();
    }

    void ensure_done() const
    {
        if (!done())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    void evacuate_result()
    {
        ensure_done();
        if (evacuated_)
            return;
        try {
            if (result != nullptr)
                result->set_value(std::move(handle.promise()).result());
            else
                static_cast<void>(std::move(handle.promise()).result());
        } catch (...) {
            failure_ = std::current_exception();
            if (result != nullptr)
                result->set_exception(failure_);
        }
        evacuated_ = true;
        if (result != nullptr)
            result->record.child = nullptr;
        destroy_frame();
    }

    void destroy_frame() noexcept
    {
        if (!handle)
            return;
        debug::unpark_task(handle.promise().id);
        handle.promise().unregister_from_deck();
        auto dying = handle;
        handle = handle_type{};
        dying.destroy();
    }

    handle_type handle;
    deed_result_state<T> * result = nullptr;
    std::exception_ptr failure_;
    bool joined_ = false;
    bool evacuated_ = false;
};

template<>
struct child_record<void> final : child_record_base
{
    using handle_type = typename task<void>::coroutine_handle;

    child_record(
        handle_type h,
        firm & owner,
        deed_result_state<void> * result)
        : handle(h)
        , result(result)
    {
        this->firm_record.owner = &owner;
        if (this->result != nullptr)
            this->result->record.child = this;
    }

    ~child_record() override
    {
        if (result != nullptr)
            result->record.child = nullptr;
        destroy_frame();
    }

    [[nodiscard]] bool done() const noexcept override
    {
        return evacuated_ || !handle || handle.done();
    }

    [[nodiscard]] bool joined() const noexcept override
    {
        return joined_;
    }

    [[nodiscard]] task<void> join() override
    {
        if (!joined_ && handle && !handle.done())
            co_await started_handle_awaiter{handle};
        joined_ = true;
        evacuate_result();
    }

    [[nodiscard]] std::exception_ptr completion_failure()
        noexcept override
    {
        if (!handle || !handle.done())
            return {};
        try {
            handle.promise().result();
        } catch (...) {
            return std::current_exception();
        }
        return {};
    }

    [[nodiscard]] std::exception_ptr failure() override
    {
        evacuate_result();
        if (result != nullptr)
            return result->failure();
        return failure_;
    }

    [[nodiscard]] bool result_contained() const noexcept override
    {
        return result != nullptr && result->record.contained;
    }

    [[nodiscard]] bool result_observed() const noexcept override
    {
        return result != nullptr && result->record.observed;
    }

    [[nodiscard]] bool result_exported() const noexcept override
    {
        return result != nullptr;
    }

    void evacuate_result_if_done() override
    {
        if (done())
            evacuate_result();
    }

    void drop_result_state(deed_result_state_base * state) noexcept override
    {
        if (state == result)
            result = nullptr;
    }

    void replace_result_state(
        deed_result_state_base * old_state,
        deed_result_state_base * new_state) noexcept override
    {
        if (old_state == result)
            result = static_cast<deed_result_state<void> *>(new_state);
    }

    void request_stop() noexcept override
    {
        if (handle)
            handle.promise().request_stop();
    }

    void ensure_done() const
    {
        if (!done())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    void evacuate_result()
    {
        ensure_done();
        if (evacuated_)
            return;
        try {
            handle.promise().result();
            if (result != nullptr)
                result->set_value();
        } catch (...) {
            failure_ = std::current_exception();
            if (result != nullptr)
                result->set_exception(failure_);
        }
        evacuated_ = true;
        if (result != nullptr)
            result->record.child = nullptr;
        destroy_frame();
    }

    void destroy_frame() noexcept
    {
        if (!handle)
            return;
        debug::unpark_task(handle.promise().id);
        handle.promise().unregister_from_deck();
        auto dying = handle;
        handle = handle_type{};
        dying.destroy();
    }

    handle_type handle;
    deed_result_state<void> * result = nullptr;
    std::exception_ptr failure_;
    bool joined_ = false;
    bool evacuated_ = false;
};

struct firm_child_slot
{
    static constexpr std::size_t inline_record_bytes = 128;

    firm_child_slot() = default;
    firm_child_slot(const firm_child_slot &) = delete;
    firm_child_slot & operator=(const firm_child_slot &) = delete;
    firm_child_slot(firm_child_slot &&) = delete;
    firm_child_slot & operator=(firm_child_slot &&) = delete;

    ~firm_child_slot()
    {
        reset();
    }

    template<typename Record, typename... Args>
        requires std::derived_from<Record, child_record_base>
    [[nodiscard]] Record & emplace(Args &&... args)
    {
        static_assert(
            sizeof(Record) <= inline_record_bytes,
            "firm child slot inline storage is too small");
        static_assert(
            alignof(Record) <= alignof(firm_child_slot),
            "firm child slot inline storage is under-aligned");

        reset();
        auto * child = ::new (storage_.data())
            Record(std::forward<Args>(args)...);
        record = child;
        return *child;
    }

    void reset() noexcept
    {
        if (record == nullptr)
            return;
        std::destroy_at(record);
        record = nullptr;
    }

    child_record_base * record = nullptr;

private:
    alignas(std::max_align_t)
        std::array<std::byte, inline_record_bytes> storage_{};
};

} // namespace detail

template<typename T>
class deed
{
public:
    deed() = default;
    deed(const deed &) = delete;
    deed & operator=(const deed &) = delete;
    deed(deed && other) noexcept(
        std::is_nothrow_move_constructible_v<
            detail::deed_result_state<T>>)
        : state_(std::move(other.state_))
    {
        other.state_.reset();
    }

    deed & operator=(deed && other) noexcept(
        std::is_nothrow_move_assignable_v<
            std::optional<detail::deed_result_state<T>>>)
    {
        if (this == &other)
            return *this;
        state_ = std::move(other.state_);
        other.state_.reset();
        return *this;
    }

    [[nodiscard]] std::exception_ptr exception() const
    {
        return state().observe_exception();
    }

    [[nodiscard]] task_id child_task_id() const
    {
        return state().record.child_task;
    }

    deed & store_result_in(std::remove_cv_t<T> & target)
        requires std::assignable_from<std::remove_cv_t<T> &, T>
    {
        state().store_result_in(target);
        return *this;
    }

    deed & store_result_in(
        deed_result_storage<std::remove_cv_t<T>> & target)
        requires std::constructible_from<std::remove_cv_t<T>, T>
    {
        state().store_result_in(target);
        return *this;
    }

    [[nodiscard]] T get() &&
    {
        return state().take_result();
    }

    [[nodiscard]] catching_deed<T> cope() &&;

private:
    friend class firm;
    friend class catching_deed<T>;

    explicit deed(std::in_place_t)
        : state_(std::in_place)
    {}

    [[nodiscard]] detail::deed_result_state<T> & state() const
    {
        if (!state_)
            throw runtime_error{"nxtrt empty deed handle"};
        return *state_;
    }

    mutable std::optional<detail::deed_result_state<T>> state_;
};

template<>
class deed<void>
{
public:
    deed() = default;
    deed(const deed &) = delete;
    deed & operator=(const deed &) = delete;
    deed(deed && other) noexcept
        : state_(std::move(other.state_))
    {
        other.state_.reset();
    }

    deed & operator=(deed && other) noexcept
    {
        if (this == &other)
            return *this;
        state_ = std::move(other.state_);
        other.state_.reset();
        return *this;
    }

    [[nodiscard]] std::exception_ptr exception() const
    {
        return state().observe_exception();
    }

    [[nodiscard]] task_id child_task_id() const
    {
        return state().record.child_task;
    }

    void get() &&
    {
        state().take_result();
    }

    [[nodiscard]] catching_deed<void> cope() &&;

private:
    friend class firm;
    friend class catching_deed<void>;

    explicit deed(std::in_place_t)
        : state_(std::in_place)
    {}

    [[nodiscard]] detail::deed_result_state<void> & state() const
    {
        if (!state_)
            throw runtime_error{"nxtrt empty deed handle"};
        return *state_;
    }

    mutable std::optional<detail::deed_result_state<void>> state_;
};

struct frame_storage_ref
{
    frame_storage_ref() = default;

    explicit frame_storage_ref(std::span<std::byte> bytes)
        : bytes(bytes)
    {}

    std::span<std::byte> bytes;
};

template<std::size_t N>
class static_frame_storage
{
public:
    [[nodiscard]] frame_storage_ref ref() noexcept
    {
        return frame_storage_ref{std::span{storage_}};
    }

    [[nodiscard]] operator frame_storage_ref() noexcept
    {
        return ref();
    }

private:
    alignas(std::max_align_t) std::array<std::byte, N == 0 ? 1 : N> storage_{};
};

class owned_frame_storage
{
public:
    owned_frame_storage() = default;

    explicit owned_frame_storage(std::size_t capacity)
        : bytes_(allocate(capacity))
        , capacity_(capacity)
    {}

    owned_frame_storage(const owned_frame_storage &) = delete;
    owned_frame_storage & operator=(const owned_frame_storage &) = delete;

    owned_frame_storage(owned_frame_storage && other) noexcept
        : bytes_(std::exchange(other.bytes_, nullptr))
        , capacity_(std::exchange(other.capacity_, 0))
    {}

    owned_frame_storage & operator=(owned_frame_storage && other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        bytes_ = std::exchange(other.bytes_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        return *this;
    }

    ~owned_frame_storage()
    {
        reset();
    }

    [[nodiscard]] frame_storage_ref ref() noexcept
    {
        return frame_storage_ref{std::span{bytes_, capacity_}};
    }

    [[nodiscard]] operator frame_storage_ref() noexcept
    {
        return ref();
    }

private:
    [[nodiscard]] static std::byte * allocate(std::size_t capacity)
    {
        if (capacity == 0)
            return nullptr;
        auto * storage = ::operator new(
            capacity,
            std::align_val_t{alignof(std::max_align_t)});
        return static_cast<std::byte *>(storage);
    }

    void reset() noexcept
    {
        if (bytes_ != nullptr)
            ::operator delete(
                bytes_,
                std::align_val_t{alignof(std::max_align_t)});
        bytes_ = nullptr;
        capacity_ = 0;
    }

    std::byte * bytes_ = nullptr;
    std::size_t capacity_ = 0;
};

struct firm_child_storage_ref
{
    firm_child_storage_ref() = default;

    explicit firm_child_storage_ref(
        std::span<detail::firm_child_slot> slots)
        : slots(slots)
    {}

    std::span<detail::firm_child_slot> slots;
};

template<std::size_t N>
class static_firm_child_storage
{
public:
    [[nodiscard]] firm_child_storage_ref ref() noexcept
    {
        return firm_child_storage_ref{
            std::span<detail::firm_child_slot>{storage_.data(), N}};
    }

    [[nodiscard]] operator firm_child_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<detail::firm_child_slot, N == 0 ? 1 : N> storage_{};
};

class owned_firm_child_storage
{
public:
    owned_firm_child_storage() = default;

    explicit owned_firm_child_storage(std::size_t capacity)
        : slots_(
            capacity == 0
                ? nullptr
                : std::make_unique<detail::firm_child_slot[]>(capacity))
        , capacity_(capacity)
    {}

    owned_firm_child_storage(const owned_firm_child_storage &) = delete;
    owned_firm_child_storage & operator=(
        const owned_firm_child_storage &) = delete;
    owned_firm_child_storage(owned_firm_child_storage &&) noexcept = default;
    owned_firm_child_storage & operator=(
        owned_firm_child_storage &&) noexcept = default;

    [[nodiscard]] firm_child_storage_ref ref() noexcept
    {
        return firm_child_storage_ref{
            std::span<detail::firm_child_slot>{slots_.get(), capacity_}};
    }

    [[nodiscard]] operator firm_child_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::unique_ptr<detail::firm_child_slot[]> slots_;
    std::size_t capacity_ = 0;
};

struct firm_deed_storage_ref
{
    firm_deed_storage_ref() = default;

    explicit firm_deed_storage_ref(
        std::span<detail::firm_deed_record> records)
        : records(records)
    {}

    std::span<detail::firm_deed_record> records;
};

template<std::size_t N>
class static_firm_deed_storage
{
public:
    [[nodiscard]] firm_deed_storage_ref ref() noexcept
    {
        return firm_deed_storage_ref{
            std::span<detail::firm_deed_record>{storage_.data(), N}};
    }

    [[nodiscard]] operator firm_deed_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<detail::firm_deed_record, N == 0 ? 1 : N> storage_{};
};

class owned_firm_deed_storage
{
public:
    owned_firm_deed_storage() = default;

    explicit owned_firm_deed_storage(std::size_t capacity)
        : records_(
            capacity == 0
                ? nullptr
                : std::make_unique<detail::firm_deed_record[]>(capacity))
        , capacity_(capacity)
    {}

    owned_firm_deed_storage(const owned_firm_deed_storage &) = delete;
    owned_firm_deed_storage & operator=(
        const owned_firm_deed_storage &) = delete;
    owned_firm_deed_storage(owned_firm_deed_storage &&) noexcept = default;
    owned_firm_deed_storage & operator=(
        owned_firm_deed_storage &&) noexcept = default;

    [[nodiscard]] firm_deed_storage_ref ref() noexcept
    {
        return firm_deed_storage_ref{
            std::span<detail::firm_deed_record>{
                records_.get(), capacity_}};
    }

    [[nodiscard]] operator firm_deed_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::unique_ptr<detail::firm_deed_record[]> records_;
    std::size_t capacity_ = 0;
};

struct firm_completion_storage_ref
{
    firm_completion_storage_ref() = default;

    explicit firm_completion_storage_ref(
        std::span<detail::child_completion> completions)
        : completions(completions)
    {}

    std::span<detail::child_completion> completions;
};

template<std::size_t N>
class static_firm_completion_storage
{
public:
    [[nodiscard]] firm_completion_storage_ref ref() noexcept
    {
        return firm_completion_storage_ref{
            std::span<detail::child_completion>{storage_.data(), N}};
    }

    [[nodiscard]] operator firm_completion_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<detail::child_completion, N == 0 ? 1 : N> storage_{};
};

class owned_firm_completion_storage
{
public:
    owned_firm_completion_storage() = default;

    explicit owned_firm_completion_storage(std::size_t capacity)
        : completions_(
            capacity == 0
                ? nullptr
                : std::make_unique<detail::child_completion[]>(capacity))
        , capacity_(capacity)
    {}

    owned_firm_completion_storage(
        const owned_firm_completion_storage &) = delete;
    owned_firm_completion_storage & operator=(
        const owned_firm_completion_storage &) = delete;
    owned_firm_completion_storage(
        owned_firm_completion_storage &&) noexcept = default;
    owned_firm_completion_storage & operator=(
        owned_firm_completion_storage &&) noexcept = default;

    [[nodiscard]] firm_completion_storage_ref ref() noexcept
    {
        return firm_completion_storage_ref{
            std::span<detail::child_completion>{
                completions_.get(), capacity_}};
    }

    [[nodiscard]] operator firm_completion_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::unique_ptr<detail::child_completion[]> completions_;
    std::size_t capacity_ = 0;
};

struct firm_join_storage_ref
{
    firm_join_storage_ref() = default;

    explicit firm_join_storage_ref(
        std::span<std::exception_ptr> failures)
        : failures(failures)
    {}

    std::span<std::exception_ptr> failures;
};

template<std::size_t N>
class static_firm_join_storage
{
public:
    [[nodiscard]] firm_join_storage_ref ref() noexcept
    {
        return firm_join_storage_ref{
            std::span<std::exception_ptr>{storage_.data(), N}};
    }

    [[nodiscard]] operator firm_join_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<std::exception_ptr, N == 0 ? 1 : N> storage_{};
};

class owned_firm_join_storage
{
public:
    owned_firm_join_storage() = default;

    explicit owned_firm_join_storage(std::size_t capacity)
        : failures_(
            capacity == 0
                ? nullptr
                : std::make_unique<std::exception_ptr[]>(capacity))
        , capacity_(capacity)
    {}

    owned_firm_join_storage(const owned_firm_join_storage &) = delete;
    owned_firm_join_storage & operator=(
        const owned_firm_join_storage &) = delete;
    owned_firm_join_storage(owned_firm_join_storage &&) noexcept = default;
    owned_firm_join_storage & operator=(
        owned_firm_join_storage &&) noexcept = default;

    [[nodiscard]] firm_join_storage_ref ref() noexcept
    {
        return firm_join_storage_ref{
            std::span<std::exception_ptr>{failures_.get(), capacity_}};
    }

    [[nodiscard]] operator firm_join_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::unique_ptr<std::exception_ptr[]> failures_;
    std::size_t capacity_ = 0;
};

struct firm_storage_ref
{
    firm_storage_ref() = default;

    firm_storage_ref(
        frame_storage_ref frames,
        firm_child_storage_ref children,
        firm_deed_storage_ref deeds,
        firm_completion_storage_ref completions,
        firm_join_storage_ref joins)
        : frames(frames)
        , children(children)
        , deeds(deeds)
        , completions(completions)
        , joins(joins)
    {}

    frame_storage_ref frames;
    firm_child_storage_ref children;
    firm_deed_storage_ref deeds;
    firm_completion_storage_ref completions;
    firm_join_storage_ref joins;
};

template<
    std::size_t FrameBytes,
    std::size_t ChildSlots,
    std::size_t JoinFailureSlots = ChildSlots,
    std::size_t CompletionSlots = ChildSlots,
    std::size_t DeedSlots = ChildSlots>
class static_firm_storage
{
public:
    [[nodiscard]] firm_storage_ref ref() noexcept
    {
        return firm_storage_ref{
            frames_, children_, deeds_, completions_, joins_};
    }

    [[nodiscard]] operator firm_storage_ref() noexcept
    {
        return ref();
    }

    [[nodiscard]] frame_storage_ref frames() noexcept
    {
        return frames_;
    }

    [[nodiscard]] firm_child_storage_ref children() noexcept
    {
        return children_;
    }

    [[nodiscard]] firm_deed_storage_ref deeds() noexcept
    {
        return deeds_;
    }

    [[nodiscard]] firm_join_storage_ref joins() noexcept
    {
        return joins_;
    }

    [[nodiscard]] firm_completion_storage_ref completions() noexcept
    {
        return completions_;
    }

private:
    static_frame_storage<FrameBytes> frames_;
    static_firm_child_storage<ChildSlots> children_;
    static_firm_deed_storage<DeedSlots> deeds_;
    static_firm_completion_storage<CompletionSlots> completions_;
    static_firm_join_storage<JoinFailureSlots> joins_;
};

namespace detail {

struct alignas(std::max_align_t) task_frame_header
{
    firm * owner = nullptr;
    std::size_t size = 0;
};

} // namespace detail

class firm_frame_arena
{
public:
    firm_frame_arena() = default;

    explicit firm_frame_arena(frame_storage_ref storage)
        : storage_(storage.bytes)
    {}

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return storage_.size();
    }

    [[nodiscard]] std::size_t used() const noexcept
    {
        return used_;
    }

    [[nodiscard]] std::size_t high_water() const noexcept
    {
        return high_water_;
    }

    [[nodiscard]] frame_storage_ref storage() const noexcept
    {
        return frame_storage_ref{storage_};
    }

    [[nodiscard]] void * allocate(firm & owner, std::size_t size)
    {
        constexpr auto alignment = alignof(detail::task_frame_header);
        auto offset = align_up(used_, alignment);
        auto total = sizeof(detail::task_frame_header) + size;
        if (offset > storage_.size() || total > storage_.size() - offset)
            throw runtime_error{"nxtrt firm frame arena is full"};

        auto * header = reinterpret_cast<detail::task_frame_header *>(
            storage_.data() + offset);
        header->owner = &owner;
        header->size = size;
        used_ = offset + total;
        high_water_ = std::max(high_water_, used_);
        return header + 1;
    }

private:
    [[nodiscard]] static constexpr std::size_t align_up(
        std::size_t value,
        std::size_t alignment) noexcept
    {
        auto mask = alignment - 1;
        return (value + mask) & ~mask;
    }

    std::span<std::byte> storage_;
    std::size_t used_ = 0;
    std::size_t high_water_ = 0;
};

template<typename T>
class catching_deed
{
public:
    catching_deed() = default;
    catching_deed(const catching_deed &) = delete;
    catching_deed & operator=(const catching_deed &) = delete;
    catching_deed(catching_deed && other) noexcept(
        std::is_nothrow_move_constructible_v<
            detail::deed_result_state<T>>)
        : state_(std::move(other.state_))
    {
        other.state_.reset();
    }

    catching_deed & operator=(catching_deed && other) noexcept(
        std::is_nothrow_move_assignable_v<
            std::optional<detail::deed_result_state<T>>>)
    {
        if (this == &other)
            return *this;
        state_ = std::move(other.state_);
        other.state_.reset();
        return *this;
    }

    [[nodiscard]] std::expected<T, std::exception_ptr> get() &&
    {
        auto & child = state();
        child.ensure_done();
        try {
            return child.take_result();
        } catch (...) {
            return std::unexpected{std::current_exception()};
        }
    }

    [[nodiscard]] task_id child_task_id() const
    {
        if (!state_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return state_->record.child_task;
    }

private:
    friend class deed<T>;

    explicit catching_deed(detail::deed_result_state<T> && state)
        : state_(std::in_place, std::move(state))
    {}

    [[nodiscard]] detail::deed_result_state<T> & state()
    {
        if (!state_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return *state_;
    }

    std::optional<detail::deed_result_state<T>> state_;
};

template<>
class catching_deed<void>
{
public:
    catching_deed() = default;
    catching_deed(const catching_deed &) = delete;
    catching_deed & operator=(const catching_deed &) = delete;
    catching_deed(catching_deed && other) noexcept
        : state_(std::move(other.state_))
    {
        other.state_.reset();
    }

    catching_deed & operator=(catching_deed && other) noexcept
    {
        if (this == &other)
            return *this;
        state_ = std::move(other.state_);
        other.state_.reset();
        return *this;
    }

    [[nodiscard]] std::expected<void, std::exception_ptr> get() &&
    {
        auto & child = state();
        child.ensure_done();
        try {
            child.take_result();
            return {};
        } catch (...) {
            return std::unexpected{std::current_exception()};
        }
    }

    [[nodiscard]] task_id child_task_id() const
    {
        if (!state_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return state_->record.child_task;
    }

private:
    friend class deed<void>;

    explicit catching_deed(detail::deed_result_state<void> && state)
        : state_(std::in_place, std::move(state))
    {}

    [[nodiscard]] detail::deed_result_state<void> & state()
    {
        if (!state_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return *state_;
    }

    std::optional<detail::deed_result_state<void>> state_;
};

template<typename T>
inline catching_deed<T> deed<T>::cope() &&
{
    if (!state_)
        throw runtime_error{"nxtrt empty deed handle"};
    state_->record.contained = true;
    auto result = catching_deed<T>{std::move(*state_)};
    state_.reset();
    return result;
}

inline catching_deed<void> deed<void>::cope() &&
{
    if (!state_)
        throw runtime_error{"nxtrt empty deed handle"};
    state_->record.contained = true;
    auto result = catching_deed<void>{std::move(*state_)};
    state_.reset();
    return result;
}

class firm
{
public:
    static constexpr std::size_t default_frame_capacity = 4 * 1024 * 1024;
    static constexpr std::size_t default_child_capacity = 4096;

    firm()
        : owned_frame_storage_(default_frame_capacity)
        , frames_(owned_frame_storage_)
        , uses_owned_frame_storage_(true)
        , owned_child_storage_(default_child_capacity)
        , child_slots_(owned_child_storage_.ref().slots)
        , uses_owned_child_storage_(true)
        , owned_deed_storage_(default_child_capacity)
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(default_child_capacity)
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(default_child_capacity)
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    explicit firm(frame_storage_ref frames)
        : frames_(frames)
        , owned_child_storage_(default_child_capacity)
        , child_slots_(owned_child_storage_.ref().slots)
        , uses_owned_child_storage_(true)
        , owned_deed_storage_(default_child_capacity)
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(default_child_capacity)
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(default_child_capacity)
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    explicit firm(firm_child_storage_ref children)
        : owned_frame_storage_(default_frame_capacity)
        , frames_(owned_frame_storage_)
        , uses_owned_frame_storage_(true)
        , child_slots_(children.slots)
        , owned_deed_storage_(children.slots.size())
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(children.slots.size())
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    firm(
        firm_child_storage_ref children,
        firm_deed_storage_ref deeds)
        : owned_frame_storage_(default_frame_capacity)
        , frames_(owned_frame_storage_)
        , uses_owned_frame_storage_(true)
        , child_slots_(children.slots)
        , deed_records_(deeds.records)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(children.slots.size())
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    firm(frame_storage_ref frames, firm_child_storage_ref children)
        : frames_(frames)
        , child_slots_(children.slots)
        , owned_deed_storage_(children.slots.size())
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(children.slots.size())
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    firm(
        frame_storage_ref frames,
        firm_child_storage_ref children,
        firm_deed_storage_ref deeds)
        : frames_(frames)
        , child_slots_(children.slots)
        , deed_records_(deeds.records)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(children.slots.size())
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    firm(
        firm_child_storage_ref children,
        firm_join_storage_ref join)
        : owned_frame_storage_(default_frame_capacity)
        , frames_(owned_frame_storage_)
        , uses_owned_frame_storage_(true)
        , child_slots_(children.slots)
        , owned_deed_storage_(children.slots.size())
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , join_failure_slots_(join.failures)
    {
        register_debug();
    }

    firm(
        frame_storage_ref frames,
        firm_child_storage_ref children,
        firm_join_storage_ref join)
        : frames_(frames)
        , child_slots_(children.slots)
        , owned_deed_storage_(children.slots.size())
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , join_failure_slots_(join.failures)
    {
        register_debug();
    }

    explicit firm(firm_storage_ref storage)
        : frames_(storage.frames)
        , child_slots_(storage.children.slots)
        , deed_records_(storage.deeds.records)
        , completion_slots_(storage.completions.completions)
        , join_failure_slots_(storage.joins.failures)
    {
        register_debug();
    }

private:
    void register_debug()
    {
        debug_id_ = debug::allocate_firm_id();
        debug::register_firm(
            debug::firm_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = child_count_,
                .stopping = stopping_,
            });
    }

public:
    ~firm()
    {
        if (debug_id_ != 0)
            debug::unregister_firm(debug_id_);
    }

    firm(const firm &) = delete;
    firm & operator=(const firm &) = delete;
    firm(firm && other) noexcept
        : owned_frame_storage_(std::move(other.owned_frame_storage_))
        , frames_(
            other.uses_owned_frame_storage_
                ? owned_frame_storage_.ref()
                : other.frames_.storage())
        , uses_owned_frame_storage_(
            std::exchange(other.uses_owned_frame_storage_, false))
        , owned_child_storage_(std::move(other.owned_child_storage_))
        , child_slots_(
            other.uses_owned_child_storage_
                ? owned_child_storage_.ref().slots
                : other.child_slots_)
        , uses_owned_child_storage_(
            std::exchange(other.uses_owned_child_storage_, false))
        , owned_deed_storage_(std::move(other.owned_deed_storage_))
        , deed_records_(
            other.uses_owned_deed_storage_
                ? owned_deed_storage_.ref().records
                : other.deed_records_)
        , uses_owned_deed_storage_(
            std::exchange(other.uses_owned_deed_storage_, false))
        , owned_completion_storage_(
            std::move(other.owned_completion_storage_))
        , completion_slots_(
            other.uses_owned_completion_storage_
                ? owned_completion_storage_.ref().completions
                : other.completion_slots_)
        , uses_owned_completion_storage_(
            std::exchange(other.uses_owned_completion_storage_, false))
        , owned_join_storage_(std::move(other.owned_join_storage_))
        , join_failure_slots_(
            other.uses_owned_join_storage_
                ? owned_join_storage_.ref().failures
                : other.join_failure_slots_)
        , uses_owned_join_storage_(
            std::exchange(other.uses_owned_join_storage_, false))
        , join_failure_count_(
            std::exchange(other.join_failure_count_, 0))
        , join_failure_high_water_(
            std::exchange(other.join_failure_high_water_, 0))
        , completion_count_(
            std::exchange(other.completion_count_, 0))
        , completion_high_water_(
            std::exchange(other.completion_high_water_, 0))
        , completion_overflow_(
            std::exchange(other.completion_overflow_, false))
        , deed_count_(std::exchange(other.deed_count_, 0))
        , deed_high_water_(
            std::exchange(other.deed_high_water_, 0))
        , child_count_(std::exchange(other.child_count_, 0))
        , child_high_water_(std::exchange(other.child_high_water_, 0))
        , stop_(std::move(other.stop_))
        , debug_id_(std::exchange(other.debug_id_, 0))
        , debug_parent_(std::exchange(other.debug_parent_, 0))
        , stopping_(std::exchange(other.stopping_, false))
    {
        other.child_slots_ = {};
        other.deed_records_ = {};
        other.completion_slots_ = {};
        other.join_failure_slots_ = {};
        debug_update();
    }
    firm & operator=(firm &&) = delete;

    [[nodiscard]] void * allocate_frame(std::size_t size)
    {
        return frames_.allocate(*this, size);
    }

    [[nodiscard]] std::size_t frame_capacity() const noexcept
    {
        return frames_.capacity();
    }

    [[nodiscard]] std::size_t frame_high_water() const noexcept
    {
        return frames_.high_water();
    }

    [[nodiscard]] std::size_t frame_used() const noexcept
    {
        return frames_.used();
    }

    [[nodiscard]] std::size_t child_capacity() const noexcept
    {
        return child_slots_.size();
    }

    [[nodiscard]] std::size_t child_count() const noexcept
    {
        return child_count_;
    }

    [[nodiscard]] std::size_t child_high_water() const noexcept
    {
        return child_high_water_;
    }

    [[nodiscard]] std::size_t deed_capacity() const noexcept
    {
        return deed_records_.size();
    }

    [[nodiscard]] std::size_t deed_high_water() const noexcept
    {
        return deed_high_water_;
    }

    [[nodiscard]] std::size_t join_failure_capacity() const noexcept
    {
        return join_failure_slots_.size();
    }

    [[nodiscard]] std::size_t join_failure_high_water() const noexcept
    {
        return join_failure_high_water_;
    }

    [[nodiscard]] std::size_t child_completion_capacity() const noexcept
    {
        return completion_slots_.size();
    }

    [[nodiscard]] std::size_t child_completion_high_water() const noexcept
    {
        return completion_high_water_;
    }

    void stop() noexcept
    {
        stopping_ = true;
        stop_.request_stop();
        for_each_child([](auto & child) {
            child.request_stop();
        });
        debug_update();
    }

    [[nodiscard]] bool stopping() const noexcept
    {
        return stopping_;
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stop_.stop_requested();
    }

    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        return stop_.get_token();
    }

    template<typename T>
    deed<T> fork(task<T> child)
    {
        auto * current = detail::current_env;
        auto * active_deck =
            current == nullptr ? nullptr : current->current_deck;
        if (current == nullptr || active_deck == nullptr)
            throw runtime_error{
                "nxtrt firm fork used without a running deck"};
        if (stopping_)
            throw runtime_error{"nxtrt firm fork used after stop"};
        if (child_count_ >= child_slots_.size())
            throw runtime_error{"nxtrt firm child storage is full"};
        if (deed_count_ >= deed_records_.size())
            throw runtime_error{"nxtrt firm deed record storage is full"};

        auto handle = child.release();
        if (!handle || handle.done())
            throw runtime_error{"nxtrt firm fork used with empty task"};

        auto result = deed<T>{std::in_place};
        auto * record = static_cast<detail::child_record<T> *>(nullptr);
        auto record_constructed = false;
        try {
            auto & promise = handle.promise();
            // Forked children outlive the call site, so they inherit the
            // current immutable environment snapshot.
            promise.env.copy_entries_from(*current);
            record = &child_slots_[child_count_]
                .template emplace<detail::child_record<T>>(
                handle,
                *this,
                &result.state());
            record_constructed = true;
            promise.observe_completion_of(*record);
            record->firm_record.task =
                active_deck->enqueue(handle, &promise);
            result.state().record.child_task =
                record->firm_record.task;
            deed_records_[deed_count_] = detail::firm_deed_record{
                .child = record->firm_record.task,
            };
        } catch (...) {
            if (record_constructed)
                child_slots_[child_count_].reset();
            else
                handle.destroy();
            throw;
        }

        ++child_count_;
        child_high_water_ = std::max(child_high_water_, child_count_);
        ++deed_count_;
        deed_high_water_ = std::max(deed_high_water_, deed_count_);
        debug_update();
        return result;
    }

    template<typename Fn, typename... Args>
        requires std::invocable<Fn, Args...>
            && is_task_v<std::invoke_result_t<Fn, Args...>>
    auto fork(Fn && fn, Args &&... args)
        -> deed<task_result_t<std::invoke_result_t<Fn, Args...>>>
    {
        return fork(std::invoke(
            std::forward<Fn>(fn),
            std::forward<Args>(args)...));
    }

    [[nodiscard]] task<void> join();

    [[nodiscard]] bool has_unjoined_children() const noexcept
    {
        for (auto i = std::size_t{0}; i < child_count_; ++i) {
            if (!child_slots_[i].record->joined())
                return true;
        }
        return false;
    }

    [[nodiscard]] debug::firm_id debug_id() const noexcept
    {
        return debug_id_;
    }

    void debug_parent(debug::firm_id parent) noexcept
    {
        debug_parent_ = parent;
        debug_update();
    }

protected:
    virtual void child_finished(
        detail::child_record_base &,
        std::exception_ptr) noexcept
    {}

private:
    friend struct detail::child_record_base;

    void report_child_finished(
        detail::child_record_base & child,
        std::exception_ptr known_failure = {}) noexcept
    {
        if (child.firm_record.completion_reported)
            return;
        child.firm_record.completion_reported = true;
        auto failure =
            known_failure ? known_failure : child.completion_failure();
        remember_child_completion(child.firm_record.task, failure);
        child_finished(
            child,
            failure);
    }

    void debug_update() const
    {
        debug::update_firm(
            debug::firm_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = child_count_,
                .stopping = stopping_,
            });
    }

    template<typename Fn>
    void for_each_child(Fn && fn) noexcept
    {
        for (auto i = std::size_t{0}; i < child_count_; ++i)
            std::invoke(fn, *child_slots_[i].record);
    }

    void remember_join_failure(std::exception_ptr failure)
    {
        if (join_failure_count_ >= join_failure_slots_.size())
            throw runtime_error{
                "nxtrt firm join failure storage is full"};
        join_failure_slots_[join_failure_count_++] = std::move(failure);
        join_failure_high_water_ =
            std::max(join_failure_high_water_, join_failure_count_);
    }

    void remember_child_completion(
        task_id child,
        std::exception_ptr failure) noexcept
    {
        if (completion_count_ >= completion_slots_.size()) {
            completion_overflow_ = true;
            return;
        }
        completion_slots_[completion_count_++] = detail::child_completion{
            .child = child,
            .failure = std::move(failure),
        };
        completion_high_water_ =
            std::max(completion_high_water_, completion_count_);
    }

    void throw_if_completion_overflow()
    {
        if (completion_overflow_)
            throw runtime_error{
                "nxtrt firm child completion storage is full"};
    }

    void clear_join_failures() noexcept
    {
        for (auto i = std::size_t{0}; i < join_failure_count_; ++i)
            join_failure_slots_[i] = {};
        join_failure_count_ = 0;
    }

    [[noreturn]] void throw_join_failures()
    {
        if (join_failure_count_ == 0)
            throw logic_error{
                "nxtrt firm throw_join_failures called without failures"};
        if (join_failure_count_ == 1)
            rethrow(join_failure_slots_[0]);

        auto exceptions = std::vector<std::exception_ptr>{};
        exceptions.reserve(join_failure_count_);
        for (auto i = std::size_t{0}; i < join_failure_count_; ++i)
            exceptions.push_back(join_failure_slots_[i]);
        throw exception_group{"firm tasks failed", std::move(exceptions)};
    }

    owned_frame_storage owned_frame_storage_;
    firm_frame_arena frames_;
    bool uses_owned_frame_storage_ = false;
    owned_firm_child_storage owned_child_storage_;
    std::span<detail::firm_child_slot> child_slots_;
    bool uses_owned_child_storage_ = false;
    owned_firm_deed_storage owned_deed_storage_;
    std::span<detail::firm_deed_record> deed_records_;
    bool uses_owned_deed_storage_ = false;
    owned_firm_completion_storage owned_completion_storage_;
    std::span<detail::child_completion> completion_slots_;
    bool uses_owned_completion_storage_ = false;
    owned_firm_join_storage owned_join_storage_;
    std::span<std::exception_ptr> join_failure_slots_;
    bool uses_owned_join_storage_ = false;
    std::size_t join_failure_count_ = 0;
    std::size_t join_failure_high_water_ = 0;
    std::size_t completion_count_ = 0;
    std::size_t completion_high_water_ = 0;
    bool completion_overflow_ = false;
    std::size_t deed_count_ = 0;
    std::size_t deed_high_water_ = 0;
    std::size_t child_count_ = 0;
    std::size_t child_high_water_ = 0;
    std::stop_source stop_;
    debug::firm_id debug_id_ = 0;
    debug::firm_id debug_parent_ = 0;
    bool stopping_ = false;
};

namespace detail {

inline void child_record_base::report_finished_from_promise() noexcept
{
    if (firm_record.owner != nullptr)
        firm_record.owner->report_child_finished(*this);
}

} // namespace detail

struct firm_key
{
    using value_type = firm *;
    static constexpr auto name = "firm";
};

inline firm * current_firm() noexcept
{
    auto value = env_get<firm_key>();
    if (!value)
        return nullptr;
    return *value;
}

inline firm & require_current_firm()
{
    auto * firm = current_firm();
    if (firm == nullptr)
        throw runtime_error{"nxtrt operation used without firm"};
    return *firm;
}

namespace detail {

inline void * allocate_task_frame(std::size_t size)
{
    if (auto * firm = current_firm())
        return firm->allocate_frame(size);

    throw runtime_error{"nxtrt task created without current firm"};
}

inline void deallocate_task_frame(void * ptr, std::size_t) noexcept
{
    if (ptr == nullptr)
        return;

    [[maybe_unused]] auto * header =
        static_cast<task_frame_header *>(ptr) - 1;
}

} // namespace detail

[[nodiscard]] inline task<void> join()
{
    co_await require_current_firm().join();
}

inline deck * current_deck() noexcept
{
    auto * env = current_env();
    return env == nullptr ? nullptr : env->current_deck;
}

inline std::stop_token current_task_stop_token() noexcept
{
    auto * env = current_env();
    if (env == nullptr || env->current_promise == nullptr)
        return {};
    return env->current_promise->stop_token();
}

inline bool task_stop_requested() noexcept
{
    auto * env = current_env();
    return env != nullptr
        && env->current_promise != nullptr
        && env->current_promise->stop_requested();
}

inline std::stop_token current_stop_token() noexcept
{
    auto * firm = current_firm();
    if (firm == nullptr)
        return current_task_stop_token();
    return firm->stop_token();
}

inline bool stop_requested() noexcept
{
    auto * firm = current_firm();
    return task_stop_requested()
        || (firm != nullptr && firm->stop_requested());
}

inline void throw_if_stop_requested()
{
    if (stop_requested())
        throw operation_cancelled{};
}

template<typename T>
deed<T> fork(task<T> child)
{
    return require_current_firm().fork(std::move(child));
}

template<typename Fn, typename... Args>
    requires std::invocable<Fn, Args...>
        && is_task_v<std::invoke_result_t<Fn, Args...>>
auto fork(Fn && fn, Args &&... args)
    -> deed<task_result_t<std::invoke_result_t<Fn, Args...>>>
{
    return require_current_firm().fork(
        std::forward<Fn>(fn),
        std::forward<Args>(args)...);
}

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

/// Awaitable adaptor that applies a synchronous transform to the result of an
/// inner awaitable, fused into `await_resume`. Readiness and suspension are
/// forwarded unchanged, so the transform rides whatever the inner awaitable
/// already does: a synchronously-ready source stays ready (no suspension), and
/// a suspending source pays exactly its own one round-trip. This is `then`
/// from the sender algebra, over any awaitable -- wishes, `hope`, tasks.
///
/// Unlike `then(task<T>, F)`, the result is a co-await-only awaitable, not a
/// `task<U>`: it cannot be forked or stored as a task. It is coherent here
/// because awaitables are single-shot (consumed at one `co_await`), unlike a
/// multiply-observed task.
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

/// Map an awaitable's result through a synchronous `fn`.
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

/// Closure form for the existing `task | adaptor` pipe: `bar() | map(f)`.
template<typename F>
[[nodiscard]] auto map(F fn)
{
    return map_closure<std::decay_t<F>>{std::move(fn)};
}

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

template<typename T, typename Cleanup>
    requires stored_task_factory<Cleanup>
        && std::is_void_v<stored_task_result_t<Cleanup>>
[[nodiscard]] task<T> finally(task<T> child, Cleanup cleanup)
{
    auto body_failure = std::exception_ptr{};
    auto cleanup_failure = std::exception_ptr{};

    if constexpr (std::is_void_v<T>) {
        try {
            co_await child;
        } catch (...) {
            body_failure = std::current_exception();
        }
    } else {
        auto result = std::optional<std::remove_cv_t<T>>{};
        try {
            result.emplace(co_await child);
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

        co_return std::move(*result);
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

template<typename F>
[[nodiscard]] auto let_value(F fn)
{
    return let_value_closure<std::decay_t<F>>{std::forward<F>(fn)};
}

template<typename Cleanup>
[[nodiscard]] auto finally(Cleanup cleanup)
{
    return finally_closure<std::decay_t<Cleanup>>{
        std::forward<Cleanup>(cleanup)};
}

template<typename T, typename Adaptor>
    requires requires(task<T> child, Adaptor adaptor) {
        std::move(adaptor)(std::move(child));
    }
[[nodiscard]] auto operator|(task<T> child, Adaptor adaptor)
{
    return std::move(adaptor)(std::move(child));
}

class stop_on_failure : public firm
{
public:
    stop_on_failure() = default;
    stop_on_failure(stop_on_failure &&) noexcept = default;
    stop_on_failure & operator=(stop_on_failure &&) = delete;

    [[nodiscard]] std::exception_ptr first_failure() const noexcept
    {
        return first_failure_;
    }

private:
    void child_finished(
        detail::child_record_base &,
        std::exception_ptr failure) noexcept override
    {
        if (!failure)
            return;
        if (!first_failure_)
            first_failure_ = failure;
        stop();
    }

    std::exception_ptr first_failure_;
};

class stop_on_success : public firm
{
public:
    stop_on_success() = default;
    stop_on_success(stop_on_success &&) noexcept = default;
    stop_on_success & operator=(stop_on_success &&) = delete;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return succeeded_;
    }

private:
    void child_finished(
        detail::child_record_base &,
        std::exception_ptr failure) noexcept override
    {
        if (failure)
            return;
        succeeded_ = true;
        stop();
    }

    bool succeeded_ = false;
};

namespace detail {

template<typename Base, typename Fn>
class firm_body : public Base
{
public:
    explicit firm_body(Fn fn)
        : fn_(std::move(fn))
    {}

    auto operator()()
    {
        return std::invoke(fn_, static_cast<Base &>(*this));
    }

private:
    Fn fn_;
};

template<typename Base, typename Fn>
[[nodiscard]] firm_body<Base, std::decay_t<Fn>> make_firm_body(Fn && fn)
{
    return firm_body<Base, std::decay_t<Fn>>{std::forward<Fn>(fn)};
}

template<typename, typename T>
using repeat_type = T;

template<typename T, typename Tuple, std::size_t... Is>
[[nodiscard]] T take_first_success_or_throw(
    Tuple & deeds,
    std::index_sequence<Is...>)
{
    auto exceptions = std::vector<std::exception_ptr>{};
    auto result = std::optional<T>{};

    auto inspect = [&](auto index) {
        if (result)
            return;

        auto value = std::move(std::get<index>(deeds)).get();
        if (value) {
            result.emplace(std::move(*value));
        } else {
            exceptions.push_back(value.error());
        }
    };

    (inspect(std::integral_constant<std::size_t, Is>{}), ...);

    if (result)
        return std::move(*result);
    throw_exceptions("wait_any tasks failed", std::move(exceptions));
}

template<typename Tuple, std::size_t... Is>
void take_first_void_success_or_throw(
    Tuple & deeds,
    std::index_sequence<Is...>)
{
    auto exceptions = std::vector<std::exception_ptr>{};
    auto succeeded = false;

    auto inspect = [&](auto index) {
        if (succeeded)
            return;

        auto value = std::move(std::get<index>(deeds)).get();
        if (value) {
            succeeded = true;
        } else {
            exceptions.push_back(value.error());
        }
    };

    (inspect(std::integral_constant<std::size_t, Is>{}), ...);

    if (succeeded)
        return;
    throw_exceptions("wait_any tasks failed", std::move(exceptions));
}

template<typename T>
task<T> stop_firm_on_completion(task<T> child)
{
    try {
        if constexpr (std::is_void_v<T>) {
            co_await child;
            require_current_firm().stop();
            co_return;
        } else {
            auto value = co_await child;
            require_current_firm().stop();
            co_return value;
        }
    } catch (...) {
        if (auto * firm = current_firm())
            firm->stop();
        throw;
    }
}

template<typename T>
[[nodiscard]] T take_deed_result(catching_deed<T> deed)
{
    auto value = std::move(deed).get();
    if (value)
        return std::move(*value);
    rethrow(value.error());
}

inline void take_deed_result(catching_deed<void> deed)
{
    auto value = std::move(deed).get();
    if (value)
        return;
    rethrow(value.error());
}

template<typename Tuple, std::size_t... Is>
[[nodiscard]] auto take_all_or_throw(
    Tuple & deeds,
    std::index_sequence<Is...>)
{
    return std::tuple{
        take_deed_result(std::move(std::get<Is>(deeds)))...,
    };
}

} // namespace detail

template<typename... Tasks>
    requires (sizeof...(Tasks) > 0)
[[nodiscard]] task<std::tuple<task_result_t<Tasks>...>>
when_all(Tasks... tasks)
{
    using deeds_type = std::tuple<catching_deed<task_result_t<Tasks>>...>;
    constexpr auto count = sizeof...(Tasks);

    auto deeds = co_await detail::make_firm_body<stop_on_failure>(
        [... tasks = std::move(tasks)](
            auto & policy) mutable -> task<deeds_type> {
            auto deeds = deeds_type{
                policy.fork(std::move(tasks)).cope()...,
            };
            co_await policy.join();
            co_return deeds;
        });

    co_return detail::take_all_or_throw(
        deeds,
        std::make_index_sequence<count>{});
}

template<std::ranges::input_range Range>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && (!std::is_void_v<
            task_result_t<std::ranges::range_value_t<Range>>>)
[[nodiscard]] task<
    std::vector<task_result_t<std::ranges::range_value_t<Range>>>>
when_all_range(Range tasks)
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    using deed_type = catching_deed<result_type>;

    auto deeds = co_await detail::make_firm_body<stop_on_failure>(
        [tasks = std::move(tasks)](auto & policy) mutable
            -> task<std::vector<deed_type>> {
            auto out = std::vector<deed_type>{};
            for (auto child : tasks)
                out.push_back(policy.fork(std::move(child)).cope());
            co_await policy.join();
            co_return out;
        });

    auto out = std::vector<result_type>{};
    out.reserve(deeds.size());
    for (auto & deed : deeds)
        out.push_back(detail::take_deed_result(std::move(deed)));
    co_return out;
}

template<std::ranges::input_range Range>
[[nodiscard]] task<void> for_each_task(Range tasks)
{
    for (auto child : tasks) {
        using child_type = std::remove_cvref_t<decltype(child)>;
        if constexpr (std::is_void_v<task_result_t<child_type>>) {
            co_await std::move(child);
        } else {
            (void)co_await std::move(child);
        }
    }
}

template<std::ranges::input_range Range>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && (!std::is_void_v<
            task_result_t<std::ranges::range_value_t<Range>>>)
[[nodiscard]] task<task_result_t<std::ranges::range_value_t<Range>>>
wait_any_range(Range tasks)
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    using deed_type = catching_deed<result_type>;

    auto deeds = co_await detail::make_firm_body<stop_on_success>(
        [tasks = std::move(tasks)](auto & policy) mutable
            -> task<std::vector<deed_type>> {
            auto out = std::vector<deed_type>{};
            for (auto child : tasks)
                out.push_back(policy.fork(std::move(child)).cope());
            if (out.empty())
                throw runtime_error{"wait_any_range used with no tasks"};
            co_await policy.join();
            co_return out;
        });

    auto exceptions = std::vector<std::exception_ptr>{};
    for (auto & deed : deeds) {
        auto value = std::move(deed).get();
        if (value)
            co_return std::move(*value);
        exceptions.push_back(value.error());
    }

    throw_exceptions("wait_any tasks failed", std::move(exceptions));
    throw logic_error{"wait_any_range returned without result"};
}

template<typename T, typename... Rest>
    requires (std::same_as<task<T>, std::remove_cvref_t<Rest>> && ...)
[[nodiscard]] task<T> wait_any(task<T> first, Rest... rest)
{
    using deeds_type =
        std::tuple<
            catching_deed<T>,
            detail::repeat_type<Rest, catching_deed<T>>...>;
    constexpr auto count = std::size_t{1 + sizeof...(Rest)};

    auto deeds = co_await detail::make_firm_body<stop_on_success>(
        [first = std::move(first),
         ... rest = std::move(rest)](
            auto & policy) mutable -> task<deeds_type> {
            auto deeds = deeds_type{
                policy.fork(std::move(first)).cope(),
                policy.fork(std::move(rest)).cope()...,
            };
            co_await policy.join();
            co_return deeds;
        });

    if constexpr (std::is_void_v<T>) {
        detail::take_first_void_success_or_throw(
            deeds,
            std::make_index_sequence<count>{});
    } else {
        co_return detail::take_first_success_or_throw<T>(
            deeds,
            std::make_index_sequence<count>{});
    }
}

[[nodiscard]] inline task<void> timeout_after(
    std::chrono::nanoseconds duration)
{
    co_await op::timeout::after(duration);
    throw timeout_error{};
}

template<typename T>
[[nodiscard]] task<T> with_timeout(
    std::chrono::nanoseconds duration,
    task<T> body)
{
    using deeds_type =
        std::tuple<catching_deed<T>, catching_deed<void>>;

    auto deeds = co_await with_firm(
        [duration, body = std::move(body)]() mutable
            -> task<deeds_type> {
            auto body_deed =
                fork(detail::stop_firm_on_completion(std::move(body)))
                    .cope();
            auto timeout_deed =
                fork(detail::stop_firm_on_completion(
                    timeout_after(duration))).cope();
            auto deeds = deeds_type{
                std::move(body_deed),
                std::move(timeout_deed),
            };
            co_await join();
            co_return deeds;
        });

    auto body_result = std::move(std::get<0>(deeds)).get();
    if (body_result) {
        if constexpr (std::is_void_v<T>) {
            co_return;
        } else {
            co_return std::move(*body_result);
        }
    }

    auto timeout_result = std::move(std::get<1>(deeds)).get();
    if (!timeout_result)
        rethrow(timeout_result.error());
    rethrow(body_result.error());

    if constexpr (std::is_void_v<T>) {
        co_return;
    } else {
        throw logic_error{"nxtrt with_timeout returned without result"};
    }
}

namespace detail {

template<typename T>
class owning_task_awaiter
{
public:
    explicit owning_task_awaiter(task<T> child)
        : child_(std::move(child))
        , inner_(child_.operator co_await())
    {}

    [[nodiscard]] bool await_ready()
    {
        return inner_.await_ready();
    }

    void await_suspend(std::coroutine_handle<> awaiting)
    {
        inner_.await_suspend(awaiting);
    }

    decltype(auto) await_resume()
    {
        return inner_.await_resume();
    }

private:
    task<T> child_;
    decltype(std::declval<task<T> &>().operator co_await()) inner_;
};

[[nodiscard]] inline std::exception_ptr unjoined_firm_children_error()
{
    try {
        throw runtime_error{
            "nxtrt firm body returned with unjoined children; "
            "co_await nxtrt::join() inside the firm body "
            "before locals captured by forked children go out of scope"};
    } catch (...) {
        return std::current_exception();
    }
}

template<stored_task_factory Fn>
[[nodiscard]] task<stored_task_result_t<Fn>>
run_firm_body(firm & firm, Fn fn)
{
    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        auto body_failure = std::exception_ptr{};
        try {
            co_await std::invoke(fn);
        } catch (...) {
            body_failure = std::current_exception();
        }
        if (!body_failure && firm.has_unjoined_children())
            body_failure = unjoined_firm_children_error();

        if (!body_failure)
            co_return;

        auto exceptions = std::vector<std::exception_ptr>{body_failure};
        firm.stop();
        try {
            co_await firm.join();
        } catch (...) {
            exceptions.push_back(std::current_exception());
        }
        throw_exceptions("firm body failed", std::move(exceptions));
    } else {
        using result_type = std::remove_cv_t<stored_task_result_t<Fn>>;
        auto result = std::optional<result_type>{};
        auto body_failure = std::exception_ptr{};
        try {
            result.emplace(co_await std::invoke(fn));
        } catch (...) {
            body_failure = std::current_exception();
        }
        if (!body_failure && firm.has_unjoined_children())
            body_failure = unjoined_firm_children_error();
        if (!body_failure)
            co_return std::move(*result);

        auto exceptions = std::vector<std::exception_ptr>{body_failure};
        firm.stop();
        try {
            co_await firm.join();
        } catch (...) {
            exceptions.push_back(std::current_exception());
        }
        throw_exceptions("firm body failed", std::move(exceptions));
    }
}

template<typename Firm>
struct firm_invoker
{
    Firm * firm = nullptr;

    auto operator()() const
    {
        return std::invoke(*firm);
    }
};

} // namespace detail

template<typename Firm>
    requires std::derived_from<Firm, firm>
        && stored_task_factory<Firm>
[[nodiscard]] task<stored_task_result_t<Firm>>
run_firm(Firm firm_scope)
{
    if (auto * parent = current_firm())
        firm_scope.debug_parent(parent->debug_id());
    auto body = detail::run_firm_body(
        firm_scope,
        detail::firm_invoker<Firm>{&firm_scope});
    auto stop_firm = [&firm_scope] {
        firm_scope.stop();
    };
    auto stop_firm_callback =
        std::stop_callback{current_task_stop_token(), stop_firm};

    auto run_bound = [body = std::move(body)]() mutable {
        return std::move(body);
    };

    if constexpr (std::is_void_v<stored_task_result_t<Firm>>) {
        co_await with_env<firm_key>(&firm_scope, std::move(run_bound));
    } else {
        co_return co_await with_env<firm_key>(
            &firm_scope,
            std::move(run_bound));
    }
}

template<typename Firm>
    requires std::derived_from<Firm, firm>
        && stored_task_factory<Firm>
[[nodiscard]] auto operator co_await(Firm firm_scope)
{
    using result_type = stored_task_result_t<Firm>;
    return detail::owning_task_awaiter<result_type>{
        run_firm(std::move(firm_scope))};
}

inline task<void> firm::join()
{
    struct join_failure_guard
    {
        firm * owner = nullptr;

        ~join_failure_guard() noexcept
        {
            if (owner != nullptr)
                owner->clear_join_failures();
        }
    };

    auto failure_guard = join_failure_guard{this};
    for (auto i = std::size_t{0}; i < child_count_; ++i) {
        auto & record = child_slots_[i].record;
        auto failure = std::exception_ptr{};
        auto collect_failure = std::exception_ptr{};
        try {
            auto & child = *record;
            co_await child.join();
            failure = child.failure();
            report_child_finished(child, failure);
            auto const exported = child.result_exported();
            if (!child.result_contained()
                && !child.result_observed()
                && !exported) {
                if (failure
                    && !(stop_requested()
                         && is_operation_cancelled(failure)))
                    collect_failure = failure;
            }
        } catch (...) {
            failure = std::current_exception();
            report_child_finished(*record, failure);
            if (!(stop_requested() && is_operation_cancelled(failure)))
                collect_failure = failure;
        }
        if (collect_failure)
            remember_join_failure(std::move(collect_failure));
    }

    throw_if_completion_overflow();

    if (join_failure_count_ != 0)
        throw_join_failures();
}

namespace detail {

template<stored_task_factory Fn>
[[nodiscard]] task<stored_task_result_t<Fn>>
with_firm_bound(Fn fn)
{
    auto firm_scope = firm{};
    if (auto * parent = current_firm())
        firm_scope.debug_parent(parent->debug_id());
    auto body = detail::run_firm_body(
        firm_scope,
        std::move(fn));
    auto stop_firm = [&firm_scope] {
        firm_scope.stop();
    };
    auto stop_firm_callback =
        std::stop_callback{current_task_stop_token(), stop_firm};

    auto run_bound = [body = std::move(body)]() mutable {
        return std::move(body);
    };

    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        co_await with_env<firm_key>(&firm_scope, std::move(run_bound));
    } else {
        co_return co_await with_env<firm_key>(
            &firm_scope,
            std::move(run_bound));
    }
}

} // namespace detail

template<typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] task<stored_task_result_t<std::decay_t<Fn>>>
with_firm(Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    return detail::with_firm_bound(
        factory_type{std::forward<Fn>(fn)});
}

template<typename T>
class root_task
{
public:
    root_task() = delete;
    root_task(const root_task &) = delete;
    root_task & operator=(const root_task &) = delete;
    root_task(root_task &&) = delete;
    root_task & operator=(root_task &&) = delete;

    template<typename Fn>
        requires stored_task_factory<std::decay_t<Fn>>
            && std::same_as<
                stored_task_result_t<std::decay_t<Fn>>,
                T>
    root_task(deck & d, Fn && fn)
        : deck_(&d)
    {
        using factory_type = std::decay_t<Fn>;
        [[maybe_unused]] auto previous_root_firm =
            env_.replace<firm_key>(&firm_);
        auto root_guard = detail::env_guard{env_, &d, nullptr};
        task_ = std::invoke(factory_type{std::forward<Fn>(fn)});
    }

    void start()
    {
        deck_->start(task_);
    }

    [[nodiscard]] task<T> & inner() noexcept
    {
        return task_;
    }

    [[nodiscard]] const task<T> & inner() const noexcept
    {
        return task_;
    }

    [[nodiscard]] firm & root_firm() noexcept
    {
        return firm_;
    }

private:
    firm firm_;
    runtime_env env_;
    task<T> task_;
    deck * deck_ = nullptr;
};

template<typename Fn>
root_task(deck &, Fn &&)
    -> root_task<stored_task_result_t<std::decay_t<Fn>>>;

template<task_factory Fn>
[[nodiscard]] task_result_t<std::invoke_result_t<Fn>>
deck::sync_wait(Fn && fn)
{
    using factory_type = std::decay_t<Fn>;

    auto root_firm = firm{};
    auto root_env = runtime_env{};
    [[maybe_unused]] auto previous_root_firm =
        root_env.replace<firm_key>(&root_firm);
    auto root_guard = detail::env_guard{root_env, this, nullptr};

    return sync_wait(with_firm(factory_type{std::forward<Fn>(fn)}));
}

namespace detail {

inline task<poll_until_result> poll_ready(op::poll wish)
{
    co_return poll_until_result{
        .events = co_await wish,
        .timed_out = false,
    };
}

inline task<poll_until_result> poll_deadline(
    std::chrono::nanoseconds duration)
{
    co_await op::timeout::after(duration);
    co_return poll_until_result{
        .events = 0,
        .timed_out = true,
    };
}

using poll_until_deeds =
    std::tuple<
        catching_deed<poll_until_result>,
        catching_deed<poll_until_result>>;

inline task<poll_until_deeds> fork_poll_until_race(
    task<poll_until_result> ready,
    task<poll_until_result> deadline)
{
    auto ready_deed =
        fork(stop_firm_on_completion(std::move(ready))).cope();
    auto deadline_deed =
        fork(stop_firm_on_completion(std::move(deadline))).cope();
    co_await join();
    co_return poll_until_deeds{
        std::move(ready_deed),
        std::move(deadline_deed),
    };
}

inline poll_until_result take_poll_until_result(poll_until_deeds & deeds)
{
    auto ready_result = std::move(std::get<0>(deeds)).get();
    auto deadline_result = std::move(std::get<1>(deeds)).get();

    if (ready_result)
        return std::move(*ready_result);
    if (deadline_result)
        return std::move(*deadline_result);

    if (!is_operation_cancelled(ready_result.error()))
        rethrow(ready_result.error());
    if (!is_operation_cancelled(deadline_result.error()))
        rethrow(deadline_result.error());
    rethrow(ready_result.error());
}

} // namespace detail

/// Wait until an fd is ready or a timeout expires.
///
/// This composes ordinary `op::poll` and `op::timeout` wishes in a child firm.
/// The winning child stops the firm, so the losing wish is cancelled through
/// the same path as any other task race.
[[nodiscard]] inline task<poll_until_result> poll_until_after(
    int fd,
    short events,
    std::chrono::nanoseconds timeout)
{
    auto ready = detail::poll_ready(op::poll{
        .fd = fd,
        .events = events,
    });
    auto deadline = detail::poll_deadline(timeout);
    auto deeds = co_await with_firm(
        [ready = std::move(ready),
         deadline = std::move(deadline)]() mutable {
            return detail::fork_poll_until_race(
                std::move(ready),
                std::move(deadline));
        });
    co_return detail::take_poll_until_result(deeds);
}

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
    auto * env = current_env();
    if (env == nullptr
        || env->current_deck != this
        || env->current_promise == nullptr)
        return {};
    return env->current_promise->id;
}

inline void deck::dump_if_requested()
{
    if (!debug::consume_signal_dump_request())
        return;

    std::cerr << "\n" << runtime_dump_text() << std::flush;
}

inline std::string deck::runtime_dump_text() const
{
    auto ready = std::vector<task_id>{};
    ready.reserve(ready_.size());
    for (auto id : ready_)
        ready.push_back(id);

    return debug::format_runtime_dump(
        debug::snapshot_firms(),
        debug::snapshot_waits(),
        std::move(ready));
}

inline task_id deck::register_task(
    std::coroutine_handle<> handle,
    detail::promise_base * promise)
{
    if (!handle || promise == nullptr)
        return {};

    if (promise->registered_deck != nullptr
        && promise->registered_deck != this)
        throw runtime_error{"nxtrt task scheduled on a different deck"};

    if (auto id = promise->id) {
        if (auto * existing = resolve(id)) {
            if (existing->promise != promise)
                throw runtime_error{"nxtrt task id collision"};
            existing->handle = handle;
            return id;
        }
        promise->id = {};
    }

    for (auto i = std::size_t{0}; i < tasks_.size(); ++i) {
        auto & row = tasks_[i];
        if (row.state != deck_task_state::vacant)
            continue;

        auto index = i + 1;
        if (index > task_id::max_index)
            throw runtime_error{"nxtrt deck task table is too large"};

        row.id = task_id::make(
            static_cast<std::uint32_t>(index),
            row.era);
        row.handle = handle;
        row.promise = promise;
        row.state = deck_task_state::live;
        promise->id = row.id;
        promise->registered_deck = this;
        return row.id;
    }

    throw runtime_error{"nxtrt deck task table is full"};
}

inline deck_task_record * deck::resolve(task_id id) noexcept
{
    if (!id)
        return nullptr;
    auto index = id.index();
    if (index == 0 || index > tasks_.size())
        return nullptr;
    auto & row = tasks_[index - 1];
    if (row.state != deck_task_state::live || row.id != id)
        return nullptr;
    return &row;
}

inline const deck_task_record * deck::resolve(task_id id) const noexcept
{
    if (!id)
        return nullptr;
    auto index = id.index();
    if (index == 0 || index > tasks_.size())
        return nullptr;
    auto const & row = tasks_[index - 1];
    if (row.state != deck_task_state::live || row.id != id)
        return nullptr;
    return &row;
}

inline void deck::unregister_task(
    task_id id,
    detail::promise_base * promise) noexcept
{
    auto * row = resolve(id);
    if (row == nullptr || row->promise != promise)
        return;

    row->id = {};
    row->handle = {};
    row->promise = nullptr;
    row->state = deck_task_state::vacant;
    ++row->era;
    if (row->era == 0)
        row->era = 1;

    if (promise != nullptr) {
        promise->id = {};
        if (promise->registered_deck == this)
            promise->registered_deck = nullptr;
    }
}

inline task_id deck::enqueue(
    std::coroutine_handle<> handle,
    detail::promise_base * promise)
{
    auto id = register_task(handle, promise);
    if (!id)
        return {};
    trace("deck enqueue task {}", id.value);
    ready_.push_back(id);
    return id;
}

inline void deck::resume_if_ready(task_id id)
{
    auto * task = resolve(id);
    if (task == nullptr)
        return;

    auto handle = task->handle;
    auto * promise = task->promise;
    if (!handle || handle.done())
        return;

    trace("deck resume task {}", id.value);
    auto env_guard = detail::env_guard{promise->env, this, promise};
    handle.resume();
}

inline void detail::promise_base::unregister_from_deck() noexcept
{
    if (registered_deck == nullptr)
        return;
    registered_deck->unregister_task(id, this);
}

inline void need::resume(deck & d) const
{
    trace("wand fulfill parked task");
    if (promise != nullptr)
        promise->clear_wait_stop_callback();
    if (promise != nullptr)
        debug::unpark_task(promise->id);
    d.enqueue(handle, promise);
}

namespace detail {

struct running_wish_context
{
    deck * active_deck = nullptr;
    wand * active_wand = nullptr;
    promise_base * running = nullptr;
};

inline running_wish_context current_wish_context() noexcept
{
    auto * current = detail::current_env;
    auto * active_deck = current == nullptr ? nullptr : current->current_deck;
    return running_wish_context{
        .active_deck = active_deck,
        .active_wand =
            active_deck == nullptr ? nullptr : active_deck->current_wand(),
        .running = current == nullptr ? nullptr : current->current_promise,
    };
}

template<typename Wish>
auto prep_wish_awaitable(Wish const & wish)
{
    auto context = current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr) {
        auto message = std::string{"nxtrt "};
        message.append(Wish::name);
        message.append(" wish awaited without a running wand");
        throw runtime_error{std::move(message)};
    }

    op::trace_wish(wish);
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        wish);
}

} // namespace detail

template<typename T>
inline void urge<T>::await_suspend(
    std::coroutine_handle<> awaiting) const
{
    auto * active_wand = source_;
    auto * current = detail::current_env;
    auto * running = current == nullptr ? nullptr : current->current_promise;
    if (active_wand == nullptr || running == nullptr)
        throw runtime_error{
            "nxtrt urge awaited without a prepared wand"};

    trace("urge suspend token={}", coin_);
    debug::park_task(
        running->id,
        coin_,
        debug::parked_wish_description(description_));
    active_wand->suspend(
        coin_,
        need{
            .handle = awaiting,
            .promise = running,
        });
    running->cancel_wait_on_stop(*active_wand, coin_);
}

namespace op {

template<awaitable_wish Wish>
inline urge<typename Wish::result_type> operator co_await(Wish const & wish)
{
    return detail::prep_wish_awaitable(wish);
}

} // namespace op

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
        auto * current = detail::current_env;
        auto * active_deck =
            current == nullptr ? nullptr : current->current_deck;
        auto * running = current == nullptr ? nullptr : current->current_promise;
        if (active_deck == nullptr || running == nullptr)
            throw runtime_error{
                "nxtrt yield awaited without a running deck"};
        active_deck->enqueue(awaiting, running);
    }

    /// No value is produced by `co_await nxtrt::yield()`.
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

} // namespace nxtrt
