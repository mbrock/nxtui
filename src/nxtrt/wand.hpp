#pragma once

#include "nxtrt/debug.hpp"
#include "nxtrt/exceptions.hpp"
#include "nxtrt/wish.hpp"
#include "nxtrt/wish_ops.hpp"

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace nxtrt {

class deck;
class wand;

namespace detail {
struct promise_base;
}

/// A suspended coroutine parked inside a wand, waiting for one wish.
///
/// The wand receives a need in `wand::suspend` and keeps it, keyed by the
/// wish's coin, until the operation settles. `resume(d)` drops the task's
/// stop callback and enqueues the coroutine on deck `d`; it never resumes
/// it inline, so a wand may call it from inside `wave` or its poll loop.
/// A need is a non-owning handle: call `resume` exactly once.
struct need
{
    void resume(deck & d) const;

    std::coroutine_handle<> handle;
    detail::promise_base * promise = nullptr;
};

template<typename T>
class urge;

/// Shared result slot between a wand and the urge awaiting it.
///
/// The wand stores either a value (`set_value`) or an exception
/// (`set_exception`) before resuming the need; `take()` rethrows the
/// exception or moves the value out, and throws `runtime_error` if neither
/// was set. The urge and the wand's execution record share ownership.
template<typename T>
class urge_state
{
public:
    using stored_type = std::remove_cv_t<T>;

    void set_value(T value)
    {
        value_.emplace(std::move(value));
    }

    void set_exception(std::exception_ptr exception) noexcept
    {
        exception_ = exception;
    }

    T take()
    {
        if (exception_)
            rethrow(exception_);
        if (!value_)
            throw runtime_error{"nxtrt urge result was never set"};
        return std::move(*value_);
    }

private:
    std::optional<stored_type> value_;
    std::exception_ptr exception_;
};

template<>
class urge_state<void>
{
public:
    void set_value() noexcept
    {
        done_ = true;
    }

    void set_exception(std::exception_ptr exception) noexcept
    {
        exception_ = exception;
    }

    void take()
    {
        if (exception_)
            rethrow(exception_);
        if (!done_)
            throw runtime_error{"nxtrt urge result was never set"};
    }

private:
    bool done_ = false;
    std::exception_ptr exception_;
};

/// Awaitable for one prepared wish; produced by `co_await` on a wish.
///
/// `await_ready` is always false. `await_suspend` parks the coroutine in
/// the wand as a @ref nxtrt::need "need" under the urge's coin and arms the
/// task's stop token so a stop request calls `wand::cancel(coin)`.
/// `await_resume` returns the value or rethrows the exception the wand
/// stored, typically `errno_error`, `interrupted_system_call`, or
/// `operation_cancelled`.
///
/// An urge must be awaited right away by the task that created it, on the
/// same deck: the wand has already recorded the execution, and an urge
/// that is never awaited leaves that record unsubmitted in the wand.
template<typename T>
class urge
{
public:
    using result_type = T;

    urge() = default;
    urge(
        wand & source,
        coin_t coin,
        std::shared_ptr<urge_state<T>> state,
        std::string description = {}) noexcept
        : source_(&source)
        , coin_(coin)
        , state_(std::move(state))
        , description_(std::move(description))
    {}

    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> awaiting) const;
    T await_resume()
    {
        if (state_ == nullptr)
            throw runtime_error{"nxtrt urge has no result state"};
        return state_->take();
    }

    [[nodiscard]] coin_t coin() const noexcept
    {
        return coin_;
    }

    [[nodiscard]] std::shared_ptr<urge_state<T>> state() const noexcept
    {
        return state_;
    }

private:
    wand * source_ = nullptr;
    coin_t coin_ = 0;
    std::shared_ptr<urge_state<T>> state_;
    std::string description_;
};

template<>
inline void urge<void>::await_resume()
{
    if (state_ == nullptr)
        throw runtime_error{"nxtrt urge has no result state"};
    state_->take();
}

namespace op {

/// Makes every wish awaitable: `co_await op::read_some{fd, buf}`.
///
/// Copies the wish into the current deck's wand via `wand::prepare` and
/// returns the urge. Throws `runtime_error` if there is no running task,
/// deck, or wand (for example, outside a deck or on a deck built without
/// a wand).
template<awaitable_wish Wish>
urge<typename Wish::result_type> operator co_await(Wish const & wish);

} // namespace op

namespace detail {

struct prepared_wish
{
    wish_variant wish;
    std::shared_ptr<void> state;
};

template<typename Wish>
[[nodiscard]] inline std::string describe_wish_for_urge(const Wish & wish)
{
    if constexpr (debug::describe_wishes)
        return op::describe_wish(wish);
    else
        return {};
}

} // namespace detail

/// Backend boundary that turns wishes into platform I/O.
///
/// A deck holds a pointer to one wand. While a task runs, awaiting a wish
/// calls `prepare`, which erases the wish into `wish_variant` and calls the
/// backend's `prep`; the urge then calls `suspend` to park the task. After
/// each deck round, `deck::run_ready` calls `wave`, where the wand submits
/// everything staged in that round. When an operation finishes, the wand
/// stores its result in the urge state and calls `need::resume`, which puts
/// the task back on the deck.
///
/// Shipped implementations are `uring_wand` (Linux io_uring), `epoll_wand`
/// (Linux epoll, the Fil-C default), and `kqueue_wand` (macOS and BSD);
/// `nxtrt::arch::wand` names the build's default. Each one also offers
/// non-virtual `poll`, `wait`, and `run_until_done` loops that drive a deck.
///
/// Contract for a backend:
/// - Each prepared wish gets an execution record with its own coin, unique
///   among live records. The record moves through prepared, parked,
///   settled, and retired (see `exec_lifecycle.hpp`).
/// - Settle each parked execution exactly once, with a value or an
///   exception, then call its need's `resume`.
/// - Retire (free) a record only when no kernel structure can still refer
///   to it, for example after an io_uring cancel CQE has drained.
/// - Everything runs on the deck's thread; a wand is not thread-safe.
///
/// See @ref rt_wand and the executable model in `nxtrt/runtime.rkt`.
class wand
{
public:
    virtual ~wand() = default;

    /// Records `wish` for submission and returns the urge that awaits it.
    ///
    /// Called by `op::operator co_await` while task `promise` is running on
    /// deck `d`. Nothing is submitted until the next `wave`.
    template<typename Wish>
    urge<typename Wish::result_type> prepare(
        deck & d,
        detail::promise_base & promise,
        Wish wish)
    {
        using result_type = typename Wish::result_type;
        auto state = std::make_shared<urge_state<result_type>>();
        auto description = detail::describe_wish_for_urge(wish);
        auto token = prep(
            d,
            promise,
            detail::prepared_wish{
                .wish = wish_variant{std::move(wish)},
                .state = state,
            });
        return urge<result_type>{
            *this,
            token,
            state,
            std::move(description)
        };
    }

    /// Parks `task` on the prepared execution named by `token`.
    ///
    /// Called from `urge::await_suspend`, right after `prep` in the same
    /// task turn. Implementations move the record from prepared to parked
    /// and ignore tokens that are not prepared. Must not complete the
    /// operation here; completion happens in `wave` or the poll loop.
    virtual void suspend(coin_t token, need task) = 0;

    /// Requests cancellation of the parked execution named by `token`.
    ///
    /// Called from the waiting task's stop callback. Must be a no-op for an
    /// execution that is not parked or is already cancelling. The wand must
    /// still settle the execution exactly once, normally with
    /// `operation_cancelled`, and resume the need: at once, at the next
    /// `wave`, or when the kernel confirms the cancel. The record must stay
    /// alive until the kernel can no longer report events for it.
    virtual void cancel(coin_t token) = 0;

    /// Submits the work staged since the last wave; called by the deck after
    /// each round.
    ///
    /// Also the wand's sync point for retiring settled records. Operations
    /// that finish synchronously here resume their needs onto `d`. Does not
    /// block waiting for completions.
    virtual void wave(deck & d) = 0;

    /// Whether file opens and stats complete without blocking the deck's
    /// thread. Wands that perform them as synchronous syscalls say no, so
    /// callers can move slow-disk work to a blocking pool instead.
    [[nodiscard]] virtual bool asynchronous_files() const noexcept
    {
        return false;
    }

protected:
    /// Backend hook behind `prepare`: create an execution record for
    /// `wish.wish`, keep `wish.state` to deliver the result, and return the
    /// record's coin. Runs inside the awaiting task; must not resume tasks.
    virtual coin_t prep(
        deck & d,
        detail::promise_base & promise,
        detail::prepared_wish wish) = 0;
};

} // namespace nxtrt
