#pragma once

#include "nxt/rt/deck.hpp"

#include <coroutine>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace nxt::rt {

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

    promise_base() noexcept
        : id(task_ids.next())
    {
        if (auto * current = detail::current_env)
            env.bindings = current->bindings;
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
            throw runtime_error{"nxt::rt task enqueued without a deck"};
        current->current_deck->enqueue(handle, this);
    }

    /// Identity assigned when the coroutine frame is created.
    task_id id;
    /// Raw coroutine handle for the awaiting task.
    std::coroutine_handle<> continuation;
    /// Awaiting task promise, used to restore ambient context when continuation runs.
    promise_base * continuation_promise = nullptr;
    /// Inheritable runtime environment captured by this coroutine frame.
    runtime_env env;
    /// Propagates stop from the task awaiting this task.
    std::unique_ptr<stop_callback_type> parent_stop_callback;

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
        throw runtime_error{"nxt::rt task result was never set"};
    }

    /// Move the completed result out of the promise.
    T && result() &&
    {
        if (std::holds_alternative<stored_type>(storage_))
            return std::move(std::get<stored_type>(storage_));
        if (std::holds_alternative<std::exception_ptr>(storage_))
            rethrow(std::get<std::exception_ptr>(storage_));
        throw runtime_error{"nxt::rt task result was never set"};
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
            auto * current = detail::current_env;
            auto * active_deck =
                current == nullptr ? nullptr : current->current_deck;
            auto * awaiting_promise =
                current == nullptr ? nullptr : current->current_promise;
            if (active_deck == nullptr || awaiting_promise == nullptr)
                throw runtime_error{
                    "nxt::rt task awaited without a running deck"};

            auto & promise = coroutine_.promise();
            promise.env.bindings = current->bindings;
            promise.set_continuation(awaiting, awaiting_promise);
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

template<typename Key, task_factory Fn>
[[nodiscard]] task<task_result_t<std::invoke_result_t<Fn>>>
with_env(typename Key::value_type value, Fn && fn)
{
    auto * current = detail::current_env;
    auto * promise = current == nullptr ? nullptr : current->current_promise;
    if (current == nullptr || promise == nullptr)
        throw runtime_error{
            "nxt::rt env binding used without runtime env"};

    struct binding_guard
    {
        detail::promise_base * promise = nullptr;
        env_binding_base * previous = nullptr;

        ~binding_guard()
        {
            if (promise != nullptr)
                promise->env.bindings = previous;
            auto * current = detail::current_env;
            if (current != nullptr && current->current_promise == promise)
                current->bindings = previous;
        }
    };

    auto binding = env_binding<Key>{promise->env.bindings, std::move(value)};
    auto restore = binding_guard{
        .promise = promise,
        .previous = promise->env.bindings,
    };
    promise->env.bindings = &binding;
    current->bindings = &binding;
    auto child = std::invoke(std::forward<Fn>(fn));

    if constexpr (std::is_void_v<task_result_t<std::invoke_result_t<Fn>>>) {
        co_await child;
    } else {
        co_return co_await child;
    }
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
                "nxt::rt zone join used without a running task"};

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
            throw runtime_error{"nxt::rt deed result already taken"};
        observed = true;
        result_taken = true;
        return std::move(handle.promise()).result();
    }

    void ensure_done() const
    {
        if (!done())
            throw runtime_error{
                "nxt::rt deed result read before zone join"};
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
            throw runtime_error{"nxt::rt deed result already taken"};
        observed = true;
        result_taken = true;
        handle.promise().result();
    }

    void ensure_done() const
    {
        if (!done())
            throw runtime_error{
                "nxt::rt deed result read before zone join"};
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
            throw runtime_error{"nxt::rt empty deed handle"};
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
            throw runtime_error{"nxt::rt empty deed handle"};
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
            throw runtime_error{"nxt::rt empty catching_deed handle"};
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
            throw runtime_error{"nxt::rt empty catching_deed handle"};
        return *record_;
    }

    std::shared_ptr<detail::child_record<void>> record_;
};

template<typename T>
inline catching_deed<T> deed<T>::cope() &&
{
    auto child = std::move(record_);
    if (!child)
        throw runtime_error{"nxt::rt empty deed handle"};
    child->contained = true;
    return catching_deed<T>{std::move(child)};
}

inline catching_deed<void> deed<void>::cope() &&
{
    auto child = std::move(record_);
    if (!child)
        throw runtime_error{"nxt::rt empty deed handle"};
    child->contained = true;
    return catching_deed<void>{std::move(child)};
}

class task_zone
{
public:
    task_zone() = default;

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
                "nxt::rt zone fork used without a running deck"};
        if (stopping_)
            throw runtime_error{"nxt::rt zone fork used after stop"};

        auto handle = child.release();
        if (!handle || handle.done())
            throw runtime_error{"nxt::rt zone fork used with empty task"};

        handle.promise().env.bindings = current->bindings;
        auto record = std::shared_ptr<detail::child_record<T>>{};
        try {
            record = std::make_shared<detail::child_record<T>>(handle);
        } catch (...) {
            handle.destroy();
            throw;
        }

        children_.push_back(record);
        active_deck->enqueue(handle, &handle.promise());
        return deed<T>{std::move(record)};
    }

    [[nodiscard]] task<void> join();

private:
    std::vector<std::shared_ptr<detail::child_record_base>> children_;
    std::stop_source stop_;
    bool stopping_ = false;
};

struct task_zone_key
{
    using value_type = task_zone *;
    static constexpr auto name = "task-zone";
};

inline task_zone * current_zone() noexcept
{
    auto * value = env_get<task_zone_key>();
    if (value == nullptr)
        return nullptr;
    return *value;
}

inline task_zone & require_current_zone()
{
    auto * zone = current_zone();
    if (zone == nullptr)
        throw runtime_error{"nxt::rt operation used without task zone"};
    return *zone;
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
        throw runtime_error{"nxt::rt operation cancelled"};
}

template<typename T>
deed<T> fork(task<T> child)
{
    return require_current_zone().fork(std::move(child));
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
                if (auto failure = child.failure())
                    exceptions.push_back(std::move(failure));
            }
        } catch (...) {
            exceptions.push_back(std::current_exception());
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
    auto env = promise->env;
    env.current_deck = &d;
    env.current_promise = promise;
    auto env_guard = detail::env_guard{env};
    handle.resume();
}

inline void parked_task::resume(deck & d) const
{
    trace("wand fulfill parked task");
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
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt manual wish awaited without a running wand"};

    trace("wish manual prepare token=" + std::to_string(token));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<int> openat_wish::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt openat wish awaited without a running wand"};

    trace("wish openat prepare path=" + path);
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<std::size_t> read_some_wish::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt read wish awaited without a running wand"};

    trace("wish read prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<std::size_t> recv_some_wish::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt recv wish awaited without a running wand"};

    trace("wish recv prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<std::size_t> send_some_wish::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt send wish awaited without a running wand"};

    trace("wish send prepare fd=" + std::to_string(fd)
        + " bytes=" + std::to_string(buffer.size()));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<void> connect_wish::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt connect wish awaited without a running wand"};

    trace("wish connect prepare fd=" + std::to_string(fd));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<int> poll_wish::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt poll wish awaited without a running wand"};

    trace("wish poll prepare fd=" + std::to_string(fd)
        + " events=" + std::to_string(events));
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<void> timeout_wish::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt timeout wish awaited without a running wand"};

    trace("wish timeout prepare");
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        *this);
}

inline waiter<poll_until_result> poll_until_wish::operator co_await() const
{
    auto context = detail::current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr)
        throw runtime_error{
            "nxt::rt poll-until wish awaited without a running wand"};

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
