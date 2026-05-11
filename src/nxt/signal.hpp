#pragma once

#include "nxtio/async-core.hpp"
#include "nxtio/scope.hpp"  // for nxt::disconnected

#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace nxt {

template<typename T> class Publisher;
template<typename T> class BoundPublisher;

namespace detail {

/// Internal shared state of a Signal.
///
/// Single-slot delivery semantics: at most one awaiter is registered at
/// any time. A new `next()` while a previous awaiter is pending will
/// overwrite the slot and resume the previous awaiter with `nullopt`.
/// A `push()` with no waiter drops the value.
///
/// `next(stop_token)` additionally unregisters the pending awaiter on
/// cancellation, which lets structured races cancel losing branches
/// before they can consume later values.
template<typename T>
struct signal_state
{
    std::mutex mutex;
    std::atomic<int> publisher_count{0};
    std::atomic<bool> closed{false};

    struct pending_slot
    {
        std::coroutine_handle<> handle{};
        std::optional<T> * result{};
    };

    pending_slot pending{};
};

/// Wake the current waiter (if any) with `value`. Returns true if a
/// waiter was woken.
template<typename T>
inline bool deliver_to_pending(
    signal_state<T> & s, std::optional<T> value)
{
    std::coroutine_handle<> h;
    {
        std::lock_guard lk(s.mutex);
        if (!s.pending.handle)
            return false;
        *s.pending.result = std::move(value);
        h = s.pending.handle;
        s.pending = {};
    }
    h.resume();
    return true;
}

template<typename T>
inline void clear_pending(
    signal_state<T> & s, std::optional<T> * result)
{
    std::lock_guard lk(s.mutex);
    if (s.pending.result == result)
        s.pending = {};
}

template<typename T>
inline bool cancel_pending(
    signal_state<T> & s, std::optional<T> * result)
{
    std::coroutine_handle<> h;
    {
        std::lock_guard lk(s.mutex);
        if (s.pending.result != result)
            return false;
        *s.pending.result = std::nullopt;
        h = s.pending.handle;
        s.pending = {};
    }
    h.resume();
    return true;
}

/// Awaitable returned by `Signal::next()`.
template<typename T>
struct next_awaiter
{
    std::shared_ptr<signal_state<T>> state;
    std::stop_token stop;
    std::optional<T> value{};
    std::unique_ptr<std::stop_callback<std::function<void()>>>
        stop_callback{};

    explicit next_awaiter(
        std::shared_ptr<signal_state<T>> s,
        std::stop_token st = {}) noexcept
        : state(std::move(s))
        , stop(std::move(st))
    {}

    next_awaiter(const next_awaiter &) = delete;
    next_awaiter & operator=(const next_awaiter &) = delete;
    next_awaiter(next_awaiter && other) noexcept
        : state(std::move(other.state))
        , stop(std::move(other.stop))
        , value(std::move(other.value))
    {}
    next_awaiter & operator=(next_awaiter &&) = delete;

    ~next_awaiter()
    {
        if (state)
            clear_pending<T>(*state, &value);
    }

    bool await_ready() const noexcept
    {
        return state->closed.load(std::memory_order_acquire)
            || stop.stop_requested();
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        std::coroutine_handle<> overwritten;
        {
            std::lock_guard lk(state->mutex);
            if (state->closed.load(std::memory_order_acquire)) {
                // Closed during suspension; we'll resume immediately
                // with no value via await_resume.
                overwritten = h;
            } else {
                if (state->pending.handle) {
                    // Pre-empt the previous awaiter — give it nullopt
                    // and resume it after we release the lock.
                    *state->pending.result = std::nullopt;
                    overwritten = state->pending.handle;
                }
                state->pending = {h, &value};
            }
        }
        if (!overwritten && stop.stop_possible()) {
            stop_callback = std::make_unique<
                std::stop_callback<std::function<void()>>>(
                stop,
                [state = state, result = &value] {
                    (void)cancel_pending<T>(*state, result);
                });
        }
        if (overwritten == h) {
            // Closed case: resume ourselves.
            h.resume();
        } else if (overwritten) {
            overwritten.resume();
        }
    }

    std::optional<T> await_resume() noexcept
    {
        return std::move(value);
    }
};

} // namespace detail

/// The consumer end of a typed event channel.
///
/// Single-slot semantics:
/// - `push()` with no waiter drops the value.
/// - `next()` while one is already pending overwrites the slot and
///   delivers `nullopt` to the previous waiter.
/// - `next(stop_token)` also unregisters and returns `nullopt` when
///   cancellation is requested.
///
/// Lifetime rules:
/// - Destroying the Signal closes the receiver side: the current
///   pending waiter (if any) wakes with `nullopt`; subsequent pushes
///   throw `disconnected`.
/// - When the last Publisher is destroyed, the sender side closes:
///   `next()` wakes with `nullopt`.
template<typename T>
class Signal
{
public:
    Signal()
        : state_(std::make_shared<detail::signal_state<T>>())
    {}

    Signal(const Signal &) = delete;
    Signal & operator=(const Signal &) = delete;

    Signal(Signal && other) noexcept = default;
    Signal & operator=(Signal && other) noexcept
    {
        if (this != &other) {
            close();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~Signal()
    {
        close();
    }

    /// Await the next value. Returns `nullopt` once the signal has been
    /// closed (receiver or last publisher gone), or when the slot is
    /// pre-empted by a subsequent `next()`.
    [[nodiscard]] detail::next_awaiter<T> next() const
    {
        return detail::next_awaiter<T>(state_);
    }

    /// Await the next value, or `nullopt` if the signal is closed,
    /// pre-empted, or `stop` is requested.
    [[nodiscard]] detail::next_awaiter<T> next(std::stop_token stop) const
    {
        return detail::next_awaiter<T>(state_, std::move(stop));
    }

    /// Create a new write-endpoint. Cheap; copies share refcount.
    Publisher<T> publisher() const;

    /// Create a write-endpoint that always pushes the same value.
    BoundPublisher<T> publisher(T value) const;

    /// True once the receiver side has been closed.
    bool closed() const noexcept
    {
        return !state_
            || state_->closed.load(std::memory_order_acquire);
    }

    /// Close the receiver side. Idempotent.
    void close() noexcept
    {
        if (!state_)
            return;
        if (!state_->closed.exchange(true, std::memory_order_acq_rel))
            (void)detail::deliver_to_pending<T>(*state_, std::nullopt);
    }

private:
    std::shared_ptr<detail::signal_state<T>> state_;

    template<typename U> friend class Publisher;
};

/// A copyable write-endpoint for a Signal. Multiple producers can hold
/// copies; the sender side closes when the last copy is destroyed.
template<typename T>
class Publisher
{
public:
    Publisher(const Publisher & other) noexcept
        : state_(other.state_)
    {
        acquire();
    }

    Publisher & operator=(const Publisher & other) noexcept
    {
        if (this != &other) {
            release();
            state_ = other.state_;
            acquire();
        }
        return *this;
    }

    Publisher(Publisher && other) noexcept
        : state_(std::move(other.state_))
    {}

    Publisher & operator=(Publisher && other) noexcept
    {
        if (this != &other) {
            release();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~Publisher()
    {
        release();
    }

    /// Deliver a value to the current waiter, if any. Drops the value
    /// if no waiter is pending. Throws `nxt::disconnected` if the
    /// Signal has been closed.
    nxt::task<void> push(T value) const
    {
        if (!state_
            || state_->closed.load(std::memory_order_acquire))
        {
            throw nxt::disconnected{};
        }
        (void)detail::deliver_to_pending<T>(
            *state_, std::optional<T>(std::move(value)));
        co_return;
    }

    bool disconnected() const noexcept
    {
        return !state_
            || state_->closed.load(std::memory_order_acquire);
    }

    BoundPublisher<T> bound(T value) const;

private:
    friend class Signal<T>;

    explicit Publisher(
        std::shared_ptr<detail::signal_state<T>> s) noexcept
        : state_(std::move(s))
    {
        acquire();
    }

    void acquire() noexcept
    {
        if (state_)
            state_->publisher_count.fetch_add(
                1, std::memory_order_acq_rel);
    }

    void release() noexcept
    {
        if (!state_)
            return;
        if (state_->publisher_count.fetch_sub(
                1, std::memory_order_acq_rel)
            == 1)
        {
            // Last publisher gone — close from the sender side, wake
            // the pending waiter with nullopt.
            if (!state_->closed.exchange(
                    true, std::memory_order_acq_rel))
            {
                (void)detail::deliver_to_pending<T>(
                    *state_, std::nullopt);
            }
        }
        state_.reset();
    }

    std::shared_ptr<detail::signal_state<T>> state_;
};

/// A publisher with a value baked in. Calling `push()` (no args)
/// pushes the bound value into the underlying signal.
template<typename T>
class BoundPublisher
{
public:
    BoundPublisher(Publisher<T> pub, T value)
        : pub_(std::move(pub))
        , value_(std::move(value))
    {}

    nxt::task<void> push() const
    {
        co_await pub_.push(value_);
    }

    bool disconnected() const noexcept
    {
        return pub_.disconnected();
    }

private:
    Publisher<T> pub_;
    T value_;
};

template<typename T>
Publisher<T> Signal<T>::publisher() const
{
    return Publisher<T>(state_);
}

template<typename T>
BoundPublisher<T> Signal<T>::publisher(T value) const
{
    return BoundPublisher<T>(publisher(), std::move(value));
}

template<typename T>
BoundPublisher<T> Publisher<T>::bound(T value) const
{
    return BoundPublisher<T>(*this, std::move(value));
}

/// Race awaitables, requesting `stop_source` once the first completes.
/// Awaitables must observe the corresponding stop token themselves.
template<typename... Awaitables>
auto select(std::stop_source stop_source, Awaitables &&... aws)
{
    return coro::when_any(
        std::move(stop_source), std::forward<Awaitables>(aws)...);
}

/// Race awaitables — return the result of whichever finishes first as a
/// `std::variant`. `void`-returning awaitables become `std::monostate`
/// alternatives.
template<typename... Awaitables>
auto select(Awaitables &&... aws)
{
    return coro::when_any(std::forward<Awaitables>(aws)...);
}

} // namespace nxt
