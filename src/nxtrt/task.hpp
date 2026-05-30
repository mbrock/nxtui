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

#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
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
    using stop_callback_type =
        std::stop_callback<std::function<void()>>;

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
        : id(task_ids.next())
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

        parent_stop_callback =
            std::make_unique<stop_callback_type>(
                token,
                [this] {
                    request_stop();
                });
    }

    void cancel_wait_on_stop(wand & w, coin_t token)
    {
        wait_stop_callback.reset();
        auto stop = stop_token();
        if (!stop.stop_possible())
            return;

        wait_stop_callback =
            std::make_unique<stop_callback_type>(
                stop,
                [&w, token] {
                    w.cancel(token);
                });
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
        if (!completion_callback)
            return;
        try {
            completion_callback();
        } catch (...) {
        }
    }

    void enqueue_self(std::coroutine_handle<> handle)
    {
        auto * current = detail::current_env;
        if (current == nullptr || current->current_deck == nullptr)
            throw runtime_error{"nxtrt task enqueued without a deck"};
        current->current_deck->enqueue(handle, this);
    }

    /// Identity assigned when the coroutine frame is created.
    task_id id;
    /// Raw coroutine handle for the awaiting task.
    std::coroutine_handle<> continuation;
    /// Awaiting task promise, used to restore ambient context when continuation runs.
    promise_base * continuation_promise = nullptr;
    /// Promise-owned ambient environment captured by this coroutine frame.
    runtime_env env;
    /// Propagates stop from the task awaiting this task.
    std::unique_ptr<stop_callback_type> parent_stop_callback;
    /// Cancels the current parked wish when this task is stopped.
    std::unique_ptr<stop_callback_type> wait_stop_callback;
    /// Optional hook used by firms to observe children at final suspend.
    std::function<void()> completion_callback;

private:
    std::stop_source stop_;
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
    [[nodiscard]] virtual std::exception_ptr failure() = 0;
    [[nodiscard]] virtual task<void> join() = 0;
    virtual void request_stop() noexcept = 0;

    bool contained = false;
    bool observed = false;
    bool completion_reported = false;
};

template<typename T>
struct child_record final : child_record_base
{
    using handle_type = typename task<T>::coroutine_handle;

    explicit child_record(handle_type h) noexcept
        : handle(h)
    {}

    ~child_record() override
    {
        if (handle)
            handle.destroy();
    }

    [[nodiscard]] bool done() const noexcept override
    {
        return !handle || handle.done();
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
    }

    [[nodiscard]] std::exception_ptr failure() override
    {
        ensure_done();
        try {
            handle.promise().result();
        } catch (...) {
            return std::current_exception();
        }
        return {};
    }

    void request_stop() noexcept override
    {
        if (handle)
            handle.promise().request_stop();
    }

    [[nodiscard]] T take_result()
    {
        ensure_done();
        if (result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        observed = true;
        result_taken = true;
        return std::move(handle.promise()).result();
    }

    void ensure_done() const
    {
        if (!done())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    handle_type handle;
    bool joined_ = false;
    bool result_taken = false;
};

template<>
struct child_record<void> final : child_record_base
{
    using handle_type = typename task<void>::coroutine_handle;

    explicit child_record(handle_type h) noexcept
        : handle(h)
    {}

    ~child_record() override
    {
        if (handle)
            handle.destroy();
    }

    [[nodiscard]] bool done() const noexcept override
    {
        return !handle || handle.done();
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
    }

    [[nodiscard]] std::exception_ptr failure() override
    {
        ensure_done();
        try {
            handle.promise().result();
        } catch (...) {
            return std::current_exception();
        }
        return {};
    }

    void request_stop() noexcept override
    {
        if (handle)
            handle.promise().request_stop();
    }

    void take_result()
    {
        ensure_done();
        if (result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        observed = true;
        result_taken = true;
        handle.promise().result();
    }

    void ensure_done() const
    {
        if (!done())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    handle_type handle;
    bool joined_ = false;
    bool result_taken = false;
};

} // namespace detail

template<typename T>
class deed
{
public:
    deed() = default;
    deed(const deed &) = delete;
    deed & operator=(const deed &) = delete;
    deed(deed &&) noexcept = default;
    deed & operator=(deed &&) noexcept = default;

    [[nodiscard]] std::exception_ptr exception() const
    {
        auto & child = record();
        child.observed = true;
        return child.failure();
    }

    [[nodiscard]] T get() &&
    {
        return record().take_result();
    }

    [[nodiscard]] catching_deed<T> cope() &&;

private:
    friend class firm;
    friend class catching_deed<T>;

    explicit deed(std::shared_ptr<detail::child_record<T>> record) noexcept
        : record_(std::move(record))
    {}

    [[nodiscard]] detail::child_record<T> & record() const
    {
        if (!record_)
            throw runtime_error{"nxtrt empty deed handle"};
        return *record_;
    }

    std::shared_ptr<detail::child_record<T>> record_;
};

template<>
class deed<void>
{
public:
    deed() = default;
    deed(const deed &) = delete;
    deed & operator=(const deed &) = delete;
    deed(deed &&) noexcept = default;
    deed & operator=(deed &&) noexcept = default;

    [[nodiscard]] std::exception_ptr exception() const
    {
        auto & child = record();
        child.observed = true;
        return child.failure();
    }

    void get() &&
    {
        record().take_result();
    }

    [[nodiscard]] catching_deed<void> cope() &&;

private:
    friend class firm;
    friend class catching_deed<void>;

    explicit deed(
        std::shared_ptr<detail::child_record<void>> record) noexcept
        : record_(std::move(record))
    {}

    [[nodiscard]] detail::child_record<void> & record() const
    {
        if (!record_)
            throw runtime_error{"nxtrt empty deed handle"};
        return *record_;
    }

    std::shared_ptr<detail::child_record<void>> record_;
};

template<typename T>
class catching_deed
{
public:
    catching_deed() = default;
    catching_deed(const catching_deed &) = delete;
    catching_deed & operator=(const catching_deed &) = delete;
    catching_deed(catching_deed &&) noexcept = default;
    catching_deed & operator=(catching_deed &&) noexcept = default;

    [[nodiscard]] std::expected<T, std::exception_ptr> get() &&
    {
        auto & child = record();
        child.ensure_done();
        try {
            return child.take_result();
        } catch (...) {
            return std::unexpected{std::current_exception()};
        }
    }

private:
    friend class deed<T>;

    explicit catching_deed(
        std::shared_ptr<detail::child_record<T>> record) noexcept
        : record_(std::move(record))
    {}

    [[nodiscard]] detail::child_record<T> & record() const
    {
        if (!record_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return *record_;
    }

    std::shared_ptr<detail::child_record<T>> record_;
};

template<>
class catching_deed<void>
{
public:
    catching_deed() = default;
    catching_deed(const catching_deed &) = delete;
    catching_deed & operator=(const catching_deed &) = delete;
    catching_deed(catching_deed &&) noexcept = default;
    catching_deed & operator=(catching_deed &&) noexcept = default;

    [[nodiscard]] std::expected<void, std::exception_ptr> get() &&
    {
        auto & child = record();
        child.ensure_done();
        try {
            child.take_result();
            return {};
        } catch (...) {
            return std::unexpected{std::current_exception()};
        }
    }

private:
    friend class deed<void>;

    explicit catching_deed(
        std::shared_ptr<detail::child_record<void>> record) noexcept
        : record_(std::move(record))
    {}

    [[nodiscard]] detail::child_record<void> & record() const
    {
        if (!record_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return *record_;
    }

    std::shared_ptr<detail::child_record<void>> record_;
};

template<typename T>
inline catching_deed<T> deed<T>::cope() &&
{
    auto child = std::move(record_);
    if (!child)
        throw runtime_error{"nxtrt empty deed handle"};
    child->contained = true;
    return catching_deed<T>{std::move(child)};
}

inline catching_deed<void> deed<void>::cope() &&
{
    auto child = std::move(record_);
    if (!child)
        throw runtime_error{"nxtrt empty deed handle"};
    child->contained = true;
    return catching_deed<void>{std::move(child)};
}

class firm
{
public:
    firm()
        : debug_id_(debug::allocate_firm_id())
    {
        debug::register_firm(
            debug::firm_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = children_.size(),
                .stopping = stopping_,
            });
    }

    ~firm()
    {
        if (debug_id_ != 0)
            debug::unregister_firm(debug_id_);
    }

    firm(const firm &) = delete;
    firm & operator=(const firm &) = delete;
    firm(firm && other) noexcept
        : children_(std::move(other.children_))
        , stop_(std::move(other.stop_))
        , debug_id_(std::exchange(other.debug_id_, 0))
        , debug_parent_(std::exchange(other.debug_parent_, 0))
        , stopping_(std::exchange(other.stopping_, false))
    {
        debug_update();
    }
    firm & operator=(firm &&) = delete;

    void stop() noexcept
    {
        stopping_ = true;
        stop_.request_stop();
        for (auto & child : children_)
            child->request_stop();
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

        auto handle = child.release();
        if (!handle || handle.done())
            throw runtime_error{"nxtrt firm fork used with empty task"};

        auto record = std::shared_ptr<detail::child_record<T>>{};
        try {
            auto & promise = handle.promise();
            // Forked children outlive the call site, so they inherit the
            // current immutable environment snapshot.
            promise.env.copy_entries_from(*current);
            record = std::make_shared<detail::child_record<T>>(handle);
            promise.completion_callback = [this, weak_record =
                std::weak_ptr<detail::child_record<T>>{record}] {
                if (auto child = weak_record.lock())
                    report_child_finished(*child);
            };
        } catch (...) {
            handle.destroy();
            throw;
        }

        children_.push_back(record);
        debug_update();
        active_deck->enqueue(handle, &handle.promise());
        return deed<T>{std::move(record)};
    }

    [[nodiscard]] task<void> join();

    [[nodiscard]] bool has_unjoined_children() const noexcept
    {
        for (auto const & child : children_) {
            if (!child->joined())
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
    void report_child_finished(detail::child_record_base & child) noexcept
    {
        if (child.completion_reported)
            return;
        child.completion_reported = true;
        child_finished(child, child.failure());
    }

    void debug_update() const
    {
        debug::update_firm(
            debug::firm_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = children_.size(),
                .stopping = stopping_,
            });
    }

    std::vector<std::shared_ptr<detail::child_record_base>> children_;
    std::stop_source stop_;
    debug::firm_id debug_id_ = 0;
    debug::firm_id debug_parent_ = 0;
    bool stopping_ = false;
};

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
    auto exceptions = std::vector<std::exception_ptr>{};
    for (auto i = std::size_t{0}; i < children_.size(); ++i) {
        auto failure = std::exception_ptr{};
        try {
            auto & child = *children_[i];
            co_await child.join();
            failure = child.failure();
            report_child_finished(child);
            auto const exported = children_[i].use_count() > 1;
            if (!child.contained && !child.observed && !exported) {
                if (failure
                    && !(stop_requested()
                         && is_operation_cancelled(failure)))
                    exceptions.push_back(failure);
            }
        } catch (...) {
            failure = std::current_exception();
            report_child_finished(*children_[i]);
            if (!(stop_requested() && is_operation_cancelled(failure)))
                exceptions.push_back(failure);
        }
    }

    if (!exceptions.empty())
        throw_exceptions("firm tasks failed", std::move(exceptions));
}

template<typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] task<stored_task_result_t<std::decay_t<Fn>>>
with_firm(Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    auto firm_scope = firm{};
    if (auto * parent = current_firm())
        firm_scope.debug_parent(parent->debug_id());
    auto body = detail::run_firm_body(
        firm_scope,
        factory_type{std::forward<Fn>(fn)});
    auto stop_firm = [&firm_scope] {
        firm_scope.stop();
    };
    auto stop_firm_callback =
        std::stop_callback{current_task_stop_token(), stop_firm};

    auto run_bound = [body = std::move(body)]() mutable {
        return std::move(body);
    };

    if constexpr (std::is_void_v<stored_task_result_t<factory_type>>) {
        co_await with_env<firm_key>(&firm_scope, std::move(run_bound));
    } else {
        co_return co_await with_env<firm_key>(
            &firm_scope,
            std::move(run_bound));
    }
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
    for (auto const & item : ready_) {
        if (item.promise != nullptr)
            ready.push_back(item.promise->id);
    }

    return debug::format_runtime_dump(
        debug::snapshot_firms(),
        debug::snapshot_waits(),
        std::move(ready));
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

inline void deck::ready_item::resume_if_ready(deck & d) const
{
    if (!handle || handle.done())
        return;

    trace("deck resume task");
    auto env_guard = detail::env_guard{promise->env, &d, promise};
    handle.resume();
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
