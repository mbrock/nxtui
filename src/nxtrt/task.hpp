#pragma once

#include "nxtrt/deck.hpp"

#include <coroutine>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <stdexcept>
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

    void cancel_wait_on_stop(wand & w, wait_token token)
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
            auto * current = detail::current_env;
            auto * active_deck =
                current == nullptr ? nullptr : current->current_deck;
            auto * awaiting_promise =
                current == nullptr ? nullptr : current->current_promise;
            if (active_deck == nullptr || awaiting_promise == nullptr)
                throw runtime_error{
                    "nxtrt task awaited without a running deck"};

            auto & promise = coroutine_.promise();
            promise.env.copy_entries_from(*current);
            promise.set_continuation(awaiting, awaiting_promise);
            if (follow_parent_stop_)
                promise.follow_stop(*awaiting_promise);
            active_deck->enqueue(coroutine_, &promise);
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
                "nxtrt zone join used without a running task"};

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
    [[nodiscard]] virtual std::exception_ptr failure() = 0;
    [[nodiscard]] virtual task<void> join() = 0;
    virtual void request_stop() noexcept = 0;

    bool contained = false;
    bool observed = false;
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

    [[nodiscard]] task<void> join() override
    {
        if (!joined && handle && !handle.done())
            co_await started_handle_awaiter{handle};
        joined = true;
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
                "nxtrt deed result read before zone join"};
    }

    handle_type handle;
    bool joined = false;
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

    [[nodiscard]] task<void> join() override
    {
        if (!joined && handle && !handle.done())
            co_await started_handle_awaiter{handle};
        joined = true;
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
                "nxtrt deed result read before zone join"};
    }

    handle_type handle;
    bool joined = false;
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
    friend class task_zone;
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
    friend class task_zone;
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

class task_zone
{
public:
    task_zone()
        : debug_id_(debug::allocate_zone_id())
    {
        debug::register_zone(
            debug::zone_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = children_.size(),
                .stopping = stopping_,
            });
    }

    ~task_zone()
    {
        debug::unregister_zone(debug_id_);
    }

    task_zone(const task_zone &) = delete;
    task_zone & operator=(const task_zone &) = delete;
    task_zone(task_zone &&) = delete;
    task_zone & operator=(task_zone &&) = delete;

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
                "nxtrt zone fork used without a running deck"};
        if (stopping_)
            throw runtime_error{"nxtrt zone fork used after stop"};

        auto handle = child.release();
        if (!handle || handle.done())
            throw runtime_error{"nxtrt zone fork used with empty task"};

        auto record = std::shared_ptr<detail::child_record<T>>{};
        try {
            auto & promise = handle.promise();
            // Forked children outlive the call site, so they inherit the
            // current immutable environment snapshot.
            promise.env.copy_entries_from(*current);
            record = std::make_shared<detail::child_record<T>>(handle);
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

    [[nodiscard]] debug::zone_id debug_id() const noexcept
    {
        return debug_id_;
    }

    void debug_parent(debug::zone_id parent) noexcept
    {
        debug_parent_ = parent;
        debug_update();
    }

private:
    void debug_update() const
    {
        debug::update_zone(
            debug::zone_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = children_.size(),
                .stopping = stopping_,
            });
    }

    std::vector<std::shared_ptr<detail::child_record_base>> children_;
    std::stop_source stop_;
    debug::zone_id debug_id_ = 0;
    debug::zone_id debug_parent_ = 0;
    bool stopping_ = false;
};

struct task_zone_key
{
    using value_type = task_zone *;
    static constexpr auto name = "task-zone";
};

inline task_zone * current_zone() noexcept
{
    auto value = env_get<task_zone_key>();
    if (!value)
        return nullptr;
    return *value;
}

inline task_zone & require_current_zone()
{
    auto * zone = current_zone();
    if (zone == nullptr)
        throw runtime_error{"nxtrt operation used without task zone"};
    return *zone;
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
    auto * zone = current_zone();
    if (zone == nullptr)
        return current_task_stop_token();
    return zone->stop_token();
}

inline bool stop_requested() noexcept
{
    auto * zone = current_zone();
    return task_stop_requested()
        || (zone != nullptr && zone->stop_requested());
}

inline void throw_if_stop_requested()
{
    if (stop_requested())
        throw operation_cancelled{};
}

template<typename T>
deed<T> fork(task<T> child)
{
    return require_current_zone().fork(std::move(child));
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

class stop_on_failure
{
public:
    stop_on_failure()
        : state_(std::make_shared<state>())
    {}

    template<typename T>
    deed<T> fork(task<T> child)
    {
        return nxtrt::fork(wrap(state_, std::move(child)));
    }

    [[nodiscard]] std::exception_ptr first_failure() const noexcept
    {
        return state_->first_failure;
    }

private:
    struct state
    {
        std::exception_ptr first_failure;
    };

    template<typename T>
    static task<T> wrap(std::shared_ptr<state> state, task<T> child)
    {
        try {
            if constexpr (std::is_void_v<T>) {
                co_await child;
                co_return;
            } else {
                co_return co_await child;
            }
        } catch (...) {
            if (!state->first_failure)
                state->first_failure = std::current_exception();
            if (auto * zone = current_zone())
                zone->stop();
            throw;
        }
    }

    std::shared_ptr<state> state_;
};

class stop_on_success
{
public:
    stop_on_success()
        : state_(std::make_shared<state>())
    {}

    template<typename T>
    deed<T> fork(task<T> child)
    {
        return nxtrt::fork(wrap(state_, std::move(child)));
    }

    [[nodiscard]] bool succeeded() const noexcept
    {
        return state_->succeeded;
    }

private:
    struct state
    {
        bool succeeded = false;
    };

    template<typename T>
    static task<T> wrap(std::shared_ptr<state> state, task<T> child)
    {
        if constexpr (std::is_void_v<T>) {
            co_await child;
            state->succeeded = true;
            require_current_zone().stop();
            co_return;
        } else {
            auto value = co_await child;
            state->succeeded = true;
            require_current_zone().stop();
            co_return value;
        }
    }

    std::shared_ptr<state> state_;
};

namespace detail {

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
task<T> stop_zone_on_completion(task<T> child)
{
    try {
        if constexpr (std::is_void_v<T>) {
            co_await child;
            require_current_zone().stop();
            co_return;
        } else {
            auto value = co_await child;
            require_current_zone().stop();
            co_return value;
        }
    } catch (...) {
        if (auto * zone = current_zone())
            zone->stop();
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

    auto deeds = co_await with_zone(
        stop_on_failure{},
        [... tasks = std::move(tasks)](
            auto & policy) mutable -> task<deeds_type> {
            co_return deeds_type{
                policy.fork(std::move(tasks)).cope()...,
            };
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

    auto deeds = co_await with_zone(
        stop_on_failure{},
        [tasks = std::move(tasks)](auto & policy) mutable
            -> task<std::vector<deed_type>> {
            auto out = std::vector<deed_type>{};
            for (auto child : tasks)
                out.push_back(policy.fork(std::move(child)).cope());
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

    auto deeds = co_await with_zone(
        stop_on_success{},
        [tasks = std::move(tasks)](auto & policy) mutable
            -> task<std::vector<deed_type>> {
            auto out = std::vector<deed_type>{};
            for (auto child : tasks)
                out.push_back(policy.fork(std::move(child)).cope());
            if (out.empty())
                throw runtime_error{"wait_any_range used with no tasks"};
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

    auto deeds = co_await with_zone(
        stop_on_success{},
        [first = std::move(first),
         ... rest = std::move(rest)](
            auto & policy) mutable -> task<deeds_type> {
            co_return deeds_type{
                policy.fork(std::move(first)).cope(),
                policy.fork(std::move(rest)).cope()...,
            };
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

    auto deeds = co_await with_zone(
        [duration, body = std::move(body)]() mutable
            -> task<deeds_type> {
            auto body_deed =
                fork(detail::stop_zone_on_completion(std::move(body)))
                    .cope();
            auto timeout_deed =
                fork(detail::stop_zone_on_completion(
                    timeout_after(duration))).cope();
            co_return deeds_type{
                std::move(body_deed),
                std::move(timeout_deed),
            };
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

template<stored_task_factory Fn>
[[nodiscard]] task<stored_task_result_t<Fn>>
run_zone_body(task_zone & zone, Fn fn)
{
    auto exceptions = std::vector<std::exception_ptr>{};

    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        try {
            co_await std::invoke(fn);
        } catch (...) {
            exceptions.push_back(std::current_exception());
            zone.stop();
        }

        try {
            co_await zone.join();
        } catch (...) {
            exceptions.push_back(std::current_exception());
        }
        if (!exceptions.empty())
            throw_exceptions("zone body failed", std::move(exceptions));
    } else {
        using result_type = std::remove_cv_t<stored_task_result_t<Fn>>;
        auto result = std::optional<result_type>{};

        try {
            result.emplace(co_await std::invoke(fn));
        } catch (...) {
            exceptions.push_back(std::current_exception());
            zone.stop();
        }

        try {
            co_await zone.join();
        } catch (...) {
            exceptions.push_back(std::current_exception());
        }
        if (!exceptions.empty())
            throw_exceptions("zone body failed", std::move(exceptions));
        co_return std::move(*result);
    }
}

} // namespace detail

inline task<void> task_zone::join()
{
    auto exceptions = std::vector<std::exception_ptr>{};
    for (auto i = std::size_t{0}; i < children_.size(); ++i) {
        try {
            auto & child = *children_[i];
            co_await child.join();
            auto const exported = children_[i].use_count() > 1;
            if (!child.contained && !child.observed && !exported) {
                if (auto failure = child.failure();
                    failure
                    && !(stop_requested()
                         && is_operation_cancelled(failure)))
                    exceptions.push_back(std::move(failure));
            }
        } catch (...) {
            auto failure = std::current_exception();
            if (!(stop_requested() && is_operation_cancelled(failure)))
                exceptions.push_back(std::move(failure));
        }
    }

    if (!exceptions.empty())
        throw_exceptions("zone tasks failed", std::move(exceptions));
}

template<typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] task<stored_task_result_t<std::decay_t<Fn>>>
with_zone(Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    auto zone = task_zone{};
    if (auto * parent = current_zone())
        zone.debug_parent(parent->debug_id());
    auto body = detail::run_zone_body(
        zone,
        factory_type{std::forward<Fn>(fn)});
    auto stop_zone = [&zone] {
        zone.stop();
    };
    auto stop_zone_callback =
        std::stop_callback{current_task_stop_token(), stop_zone};

    auto run_bound = [body = std::move(body)]() mutable {
        return std::move(body);
    };

    if constexpr (std::is_void_v<stored_task_result_t<factory_type>>) {
        co_await with_env<task_zone_key>(&zone, std::move(run_bound));
    } else {
        co_return co_await with_env<task_zone_key>(
            &zone,
            std::move(run_bound));
    }
}

template<typename Policy, typename Fn>
    requires stored_policy_task_factory<std::decay_t<Fn>, Policy>
[[nodiscard]] task<stored_policy_task_result_t<std::decay_t<Fn>, Policy>>
with_zone(Policy policy, Fn && fn)
{
    using factory_type = std::decay_t<Fn>;

    auto run_with_policy =
        [policy = std::move(policy),
         fn = factory_type{std::forward<Fn>(fn)}]() mutable {
        return std::invoke(fn, policy);
    };

    if constexpr (
        std::is_void_v<stored_policy_task_result_t<factory_type, Policy>>) {
        co_await with_zone(std::move(run_with_policy));
    } else {
        co_return co_await with_zone(std::move(run_with_policy));
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
        fork(stop_zone_on_completion(std::move(ready))).cope();
    auto deadline_deed =
        fork(stop_zone_on_completion(std::move(deadline))).cope();
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
/// This composes ordinary `op::poll` and `op::timeout` wishes in a child zone.
/// The winning child stops the zone, so the losing wish is cancelled through
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
    auto deeds = co_await with_zone(
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
        debug::snapshot_zones(),
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

inline void parked_task::resume(deck & d) const
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

} // namespace detail

template<typename T>
inline void waiter<T>::await_suspend(
    std::coroutine_handle<> awaiting) const
{
    auto * active_wand = source_;
    auto * current = detail::current_env;
    auto * running = current == nullptr ? nullptr : current->current_promise;
    if (active_wand == nullptr || running == nullptr)
        throw runtime_error{
            "nxtrt waiter awaited without a prepared wand"};

    trace("waiter suspend token=" + std::to_string(token_));
    debug::park_task(running->id, token_, description_);
    active_wand->suspend(
        token_,
        parked_task{
            .handle = awaiting,
            .promise = running,
        });
    running->cancel_wait_on_stop(*active_wand, token_);
}

inline waiter<void> op::manual::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt manual wish awaited without a running wand"};

    trace("wish manual prepare token=" + std::to_string(token));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<int> op::openat::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt openat wish awaited without a running wand"};

    trace("wish openat prepare path=" + path);
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

#if defined(__linux__)
inline waiter<statx_result> op::statx::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt statx wish awaited without a running wand"};

    trace("wish statx prepare path=" + path);
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<std::size_t> op::getdents64::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt getdents64 wish awaited without a running wand"};

    trace("wish getdents64 prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<piped_child> op::spawn_piped::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt spawn-piped wish awaited without a running wand"};

    trace("wish spawn-piped prepare argv=" + std::to_string(argv.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<pty_child> op::spawn_pty::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt spawn-pty wish awaited without a running wand"};

    trace("wish spawn-pty prepare argv=" + std::to_string(argv.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<child_result> op::wait_child::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt wait-child wish awaited without a running wand"};

    trace("wish wait-child prepare pidfd=" + std::to_string(pidfd));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<void> op::signal_child::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt signal-child wish awaited without a running wand"};

    trace("wish signal-child prepare pidfd=" + std::to_string(pidfd)
        + " signal=" + std::to_string(signal));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}
#endif

inline waiter<std::size_t> op::read_some::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt read wish awaited without a running wand"};

    trace("wish read prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<std::size_t> op::write_some::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt write wish awaited without a running wand"};

    trace("wish write prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<std::size_t> op::recv_some::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt recv wish awaited without a running wand"};

    trace("wish recv prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<std::size_t> op::send_some::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt send wish awaited without a running wand"};

    trace("wish send prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<void> op::connect::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt connect wish awaited without a running wand"};

    trace("wish connect prepare fd=" + std::to_string(fd));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<int> op::accept::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt accept wish awaited without a running wand"};

    trace("wish accept prepare fd=" + std::to_string(fd));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<int> op::poll::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt poll wish awaited without a running wand"};

    trace("wish poll prepare fd=" + std::to_string(fd)
        + " events=" + std::to_string(events));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<void> op::timeout::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt timeout wish awaited without a running wand"};

    trace("wish timeout prepare");
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<poll_until_result> op::poll_until::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxtrt poll-until wish awaited without a running wand"};

    trace("wish poll-until prepare fd=" + std::to_string(fd)
        + " events=" + std::to_string(events));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
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
