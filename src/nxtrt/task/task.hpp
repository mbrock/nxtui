#pragma once

// Owning task handles and immediately ready or pending hopes.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/promise.hpp"

namespace nxtrt {

/// A synchronous, one-shot completion registration. Owns the callable, not
/// the task. It may move while connected; destruction disconnects it.
/// Callbacks run in the completing task's context and must not destroy that
/// task or resume another coroutine inline. Queue any continuation instead.
template<typename Fn>
    requires std::is_nothrow_invocable_v<Fn &>
             && std::same_as<std::invoke_result_t<Fn &>, void>
             && std::is_nothrow_move_constructible_v<Fn>
class [[nodiscard]] completion_link : private detail::completion_observer
{
public:
    /// Construct unbound so storage can be prepared before starting tasks.
    explicit completion_link(Fn fn) noexcept
        : fn_(std::move(fn))
    {
    }

    completion_link(const completion_link &) = delete;
    completion_link & operator=(const completion_link &) = delete;
    completion_link & operator=(completion_link &&) = delete;

    completion_link(completion_link && other) noexcept
        : fn_(std::move(other.fn_))
        , promise_(std::exchange(other.promise_, nullptr))
    {
        if (promise_ != nullptr)
            promise_->completion = this;
    }

    ~completion_link()
    {
        disconnect();
    }

    /// Notify inline if already complete, otherwise at final suspension.
    /// Each connection notifies once; only one observer may be connected.
    template<typename T>
    void connect(task<T> & source)
    {
        if (promise_ != nullptr)
            throw runtime_error{
                "nxtrt completion link is already connected"};
        auto handle = source.handle();
        if (!handle)
            throw runtime_error{"nxtrt completion link on an empty task"};
        if (handle.done()) {
            std::invoke(fn_);
            return;
        }
        handle.promise().observe_completion_of(*this);
        promise_ = &handle.promise();
    }

    void disconnect() noexcept
    {
        if (promise_ != nullptr) {
            promise_->completion = nullptr;
            promise_ = nullptr;
        }
    }

    [[nodiscard]] bool connected() const noexcept
    {
        return promise_ != nullptr;
    }

private:
    void task_completed() noexcept override
    {
        disconnect();
        std::invoke(fn_);
    }

    void task_destroyed() noexcept override
    {
        promise_ = nullptr;
    }

    Fn fn_;
    detail::promise_base * promise_ = nullptr;
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

    /// Register a non-suspending callback without starting this task. Keep
    /// the returned link alive until notification, or destroy it to detach.
    /// An already-completed task calls fn before this function returns.
    template<typename Fn>
        requires requires { typename completion_link<Fn>; }
    [[nodiscard]] auto on_completed(Fn fn) &
    {
        auto link = completion_link<Fn>{std::move(fn)};
        link.connect(*this);
        return link;
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

    /// Transfer pending execution to an owner that supplies its own scheduling,
    /// completion and cancellation protocol (rather than awaiting this hope).
    [[nodiscard]] task<T> take_pending() &&
    {
        if (is_ready())
            throw runtime_error{"nxtrt hope is not pending"};
        return std::move(std::get<task<T>>(state_));
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

    [[nodiscard]] task<void> take_pending() &&
    {
        if (is_ready())
            throw runtime_error{"nxtrt hope is not pending"};
        return std::move(std::get<task<void>>(state_));
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

} // namespace nxtrt
