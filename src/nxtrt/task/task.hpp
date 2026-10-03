#pragma once

// Owning task handles and immediately ready or pending hopes.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/promise.hpp"

namespace nxtrt {

/// A one-shot callback that runs when a task reaches final suspension.
///
/// The link owns the callable `fn`, never the task. Connecting does not
/// start the task: the task still runs only when a deck resumes it (or a
/// `co_await` schedules it). A task accepts at most one completion observer
/// at a time, and each connection notifies at most once; after notifying,
/// the link is disconnected.
///
/// `fn` runs synchronously inside the completing task's final suspension,
/// before the task's awaiting continuation (if any) is queued, with the
/// completing task as the deck's current task. It may inspect the task's
/// result through `task::result()`, but it must not destroy that task or
/// resume another coroutine inline; queue any follow-up work instead.
///
/// The link may move while connected; the registration follows it.
/// Destroying the link, or calling `disconnect()`, detaches it. Destroying
/// the task first detaches the link silently, without calling `fn`.
/// Everything is confined to one deck thread.
///
/// Groups (`settle` and friends) use these links to observe their jobs, so
/// a task passed to a group must not already have a link connected.
///
/// @code
/// auto child = make_child();
/// auto link = child.on_completed([&]() noexcept { ++finished; });
/// deck.start(child);
/// deck.run_until_idle();
/// @endcode
template<typename Fn>
    requires std::is_nothrow_invocable_v<Fn &>
             && std::same_as<std::invoke_result_t<Fn &>, void>
             && std::is_nothrow_move_constructible_v<Fn>
class [[nodiscard]] completion_link final : private detail::completion_observer
{
public:
    /// Construct an unconnected link, so storage for links can exist before
    /// any observed task starts. Call `connect()` to attach it.
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

    /// Attach to `source`. If `source` is already done, call `fn` before
    /// returning and stay disconnected; otherwise call it at final
    /// suspension.
    ///
    /// Throws `runtime_error` if this link is already connected, if
    /// `source` is empty, or if `source` already has another observer.
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

    /// Detach without calling `fn`. Safe to call when not connected.
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

/// A lazy coroutine that produces a `T`, owned uniquely by this handle.
///
/// Any function that returns `task<T>` and uses `co_await` or `co_return`
/// is a task coroutine. Calling it only allocates the coroutine frame; the
/// body does not run until a @ref nxtrt::deck "deck" resumes it. A task is
/// scheduled by awaiting it from another task, by `deck::start`, by a group
/// such as `settle`, or as the root of `deck::sync_wait`. See @ref rt_task.
///
/// Ownership: the handle is move-only and destroying it destroys the frame,
/// whether or not the coroutine finished. A task that is queued on its deck
/// is dropped from the queue, but a task parked on a wish must not be
/// destroyed while the wand still holds it. `release()` hands the frame to
/// low-level code instead.
///
/// Awaiting: `co_await t` from a running task makes the awaiting task the
/// continuation, copies the awaiting task's ambient environment into `t`,
/// makes `t` follow the awaiting task's stop requests, and queues `t` on the
/// same deck. When `t` finishes, the continuation is queued, never resumed
/// inline, so it runs in a later step of the deck. The await then returns
/// the value or rethrows the exception `t` ended with. Awaiting a task that
/// is already done returns its result without suspending. Await a task at
/// most once, never when it is empty, and not after it was started some
/// other way.
///
/// Cancellation: `request_stop()` sets the task's stop flag. If the task is
/// parked on a wish, the wish is cancelled through its wand; otherwise the
/// task sees the request through `stop_requested()` and decides what to do.
/// Stop requests also flow to whatever the task is currently awaiting.
/// `shield` breaks that flow for one child.
///
/// A task and everything it awaits belong to one deck and one thread.
template<typename T>
class [[nodiscard]] task
{
public:
    /// Promise stored in the coroutine frame of a `task<T>` function.
    using promise_type = detail::promise<T>;
    using coroutine_handle = std::coroutine_handle<promise_type>;

    /// Awaiter returned by `co_await task`. See the class comment for what
    /// awaiting does. `follow_parent_stop = false` is what `shield` uses.
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

        /// Store `awaiting` as the child's continuation and queue the child
        /// on the current deck. Throws `runtime_error` outside a running
        /// deck task.
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            task::splice_handle(coroutine_, awaiting, follow_parent_stop_);
        }

        /// Return (move out) the child's value, or rethrow its exception.
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

    /// An empty task that owns no frame. `done()` is true for it.
    task() noexcept = default;

    /// Adopt `coroutine`. Called by the promise when a task coroutine is
    /// created; ordinary code gets tasks by calling task functions.
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

    /// The deck registry id of this task. Empty for an empty task and for a
    /// task that has never been scheduled; ids are assigned when a deck
    /// first queues the task.
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

    /// Register `fn` to run when this task completes, without starting it.
    ///
    /// Returns the connected @ref nxtrt::completion_link "completion_link";
    /// keep it alive until notification, or destroy it to detach. If this
    /// task is already done, `fn` runs before this function returns. `fn`
    /// must be `noexcept`, return `void`, and be nothrow-movable. Throws
    /// `runtime_error` for an empty task or one that already has an
    /// observer.
    template<typename Fn>
        requires requires { typename completion_link<Fn>; }
    [[nodiscard]] auto on_completed(Fn fn) &
    {
        auto link = completion_link<Fn>{std::move(fn)};
        link.connect(*this);
        return link;
    }

    /// Ask this task to stop. Returns true if this call made the request,
    /// false if stop was already requested or the task is empty.
    ///
    /// The request is cooperative. It cancels the wish the task is parked
    /// on (if any) and propagates to the task it is awaiting; the task then
    /// usually fails with `operation_cancelled`, but it may also finish
    /// normally. A task that has not started still starts when its deck
    /// resumes it, with the stop flag already set.
    bool request_stop() noexcept
    {
        if (!coroutine_)
            return false;
        return coroutine_.promise().request_stop();
    }

    /// True once `request_stop()` was called on this task, directly or
    /// through propagation from the task awaiting it.
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

    /// Give up ownership of the frame and return its handle, leaving this
    /// task empty. The caller becomes responsible for destroying the frame
    /// (after it is done, or when nothing can resume it). Low-level runtime
    /// machinery only.
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

    /// Queue this task on the current deck with `awaiting` as its
    /// continuation, exactly as `co_await` does: the env snapshot is copied
    /// in, stop follows the awaiting task unless `follow_stop` is false, and
    /// `awaiting` is queued (not resumed inline) when this task completes.
    ///
    /// This is the splice primitive for custom awaitables. When the
    /// synchronous fast path in `await_ready()` misses, keep a delegate task
    /// in the awaitable and call `splice_onto(awaiting)` from
    /// `await_suspend()`; read the result with `result()` in
    /// `await_resume()`. The delegate frame must outlive the suspension, so
    /// the awaitable must own the task rather than splice a temporary, hence
    /// the lvalue-ref qualifier. @ref nxtrt::hope "hope" is built this way.
    ///
    /// Throws `runtime_error` if this task is empty or no deck task is
    /// running.
    void splice_onto(
        std::coroutine_handle<> awaiting,
        bool follow_stop = true) &
    {
        if (!coroutine_)
            throw runtime_error{"nxtrt splice_onto on an empty task"};
        splice_handle(coroutine_, awaiting, follow_stop);
    }

    /// Read the result of a completed task: a reference to the stored value
    /// (nothing for `task<void>`), or rethrow the stored exception. Throws
    /// `runtime_error` if the task has not produced a result yet. The task
    /// must not be empty.
    decltype(auto) result() &
    {
        return coroutine_.promise().result();
    }

    /// Like `result() &`, but returns the stored value as an rvalue so the
    /// caller can move it out.
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

/// A result that is either ready now or still pending as a `task<T>`.
///
/// Awaiting a ready hope never suspends and returns the value inline.
/// Awaiting a pending hope splices its task onto the awaiting coroutine
/// (see `task::splice_onto`) and returns or rethrows the task's result.
///
/// This is the cheap path for buffered I/O: a stream's read is a plain
/// function that returns `hope<T>::ready(...)` on a buffer hit, with no
/// coroutine frame and no suspension, and returns a `task<T>` that performs
/// real reads only on a miss.
///
/// A hope owns its pending task and is awaited once. Pending work is lazy:
/// it starts only when the hope is awaited (or when `take_pending()` hands
/// the task to another owner). The constructor's `follow_stop` decides
/// whether the pending task follows the awaiting task's stop requests.
///
/// @code
/// nxtrt::hope<int> next()
/// {
///     if (buffered)
///         return nxtrt::hope<int>::ready(take_buffered());
///     return refill_and_take(); // a task<int>
/// }
/// @endcode
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

    /// True if the value is available now.
    [[nodiscard]] bool is_ready() const noexcept
    {
        return await_ready();
    }

    /// Move the ready value out. Throws `runtime_error` if pending.
    T take_ready()
    {
        if (auto * value = std::get_if<T>(&state_))
            return std::move(*value);
        throw runtime_error{"nxtrt hope is not ready"};
    }

    /// Move the pending task out, for an owner that schedules, observes and
    /// cancels it itself instead of awaiting this hope (a pool or group, for
    /// example). Throws `runtime_error` if the hope is ready.
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

/// `hope` for operations without a value: ready (already done) or a pending
/// `task<void>`. Same contract as the primary template.
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
