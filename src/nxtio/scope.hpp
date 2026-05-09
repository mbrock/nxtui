#pragma once

// Structured concurrency primitives for libcoro
//
// Design principles:
// 1. Use std::stop_source/stop_token as the primary cancellation mechanism
// 2. Leverage libcoro's when_any(stop_source, tasks...) for automatic cleanup
// 3. Provide RAII handles (publication/subscription) that close channels on destruction
// 4. Keep it minimal - don't duplicate what libcoro already does well
//
// Usage pattern:
//   nxt::scope s(scheduler);            // Creates a cancellation scope
//   nxt::channel<Event> ch;             // Channel for producer-consumer
//
//   co_await s.run(                     // Runs tasks until one completes, then cancels
//       producer(nxt::publish(ch, s)),  // Producer gets publication handle
//       consumer(nxt::subscribe(ch, s)) // Consumer gets subscription handle
//   );
//
// When producer finishes: channel closes, consumer sees nullopt
// When consumer finishes: channel closes, producer sees disconnected{}
// When scope cancelled: both see cancelled{}

#include <stop_token>
#include <optional>
#include <functional>
#include <atomic>
#include <vector>

#include <coro/when_any.hpp>

#include "nxtio/async.hpp"

namespace nxt {

/// Exception thrown when an operation is cancelled.
struct cancelled : std::exception
{
    const char * what() const noexcept override
    {
        return "operation cancelled";
    }
};

/// Exception thrown when trying to push to a channel with no receiver.
struct disconnected : std::exception
{
    const char * what() const noexcept override
    {
        return "channel disconnected";
    }
};

/// A structured concurrency scope.
///
/// Provides:
/// - A stop_source for cancellation signaling
/// - A scheduler reference for spawning work
/// - run() method that uses libcoro's when_any with automatic cancellation
///
/// The scope cancels itself on destruction, ensuring child operations don't outlive it.
class scope
{
public:
    explicit scope(io_scheduler & sched)
        : sched_(sched)
    {
    }

    /// Create a scope with a parent stop token.
    /// When the parent is cancelled, this scope is also cancelled.
    scope(io_scheduler & sched, std::stop_token parent_token)
        : sched_(sched)
    {
        if (parent_token.stop_possible()) {
            parent_callback_.emplace(parent_token, [this] { cancel(); });
        }
    }

    ~scope()
    {
        cancel();
    }

    // Non-copyable, non-moveable - scopes are tied to their stack frame
    scope(const scope &) = delete;
    scope & operator=(const scope &) = delete;
    scope(scope &&) = delete;
    scope & operator=(scope &&) = delete;

    /// Request cancellation of all operations bound to this scope.
    void cancel()
    {
        stop_source_.request_stop();
    }

    /// Check if cancellation has been requested.
    [[nodiscard]] bool cancelled() const noexcept
    {
        return stop_source_.stop_requested();
    }

    /// Get the stop token for manual checking or passing to other APIs.
    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        return stop_source_.get_token();
    }

    /// Get the stop source (for libcoro's when_any).
    [[nodiscard]] std::stop_source & stop_source() noexcept
    {
        return stop_source_;
    }

    /// Access the scheduler.
    [[nodiscard]] io_scheduler & scheduler() noexcept
    {
        return sched_;
    }

    /// Throw cancelled{} if the scope has been cancelled.
    void check() const
    {
        if (cancelled())
            throw nxt::cancelled{};
    }

    /// Run tasks until one completes, then cancel the scope.
    /// Uses libcoro's when_any with stop_source for automatic cancellation.
    template<typename... Tasks>
    auto run(Tasks &&... tasks)
    {
        return coro::when_any(stop_source_, std::forward<Tasks>(tasks)...);
    }

    /// Run tasks from a vector until one completes, then cancel the scope.
    auto run(std::vector<task<>> tasks)
    {
        return coro::when_any(stop_source_, std::move(tasks));
    }

private:
    io_scheduler & sched_;
    std::stop_source stop_source_;
    std::optional<std::stop_callback<std::function<void()>>> parent_callback_;
};

/// A channel for communicating between coroutines.
/// Owns the underlying queue and tracks sender/receiver state.
template<typename T>
class channel
{
public:
    channel() = default;

    // Non-copyable, moveable
    channel(const channel &) = delete;
    channel & operator=(const channel &) = delete;
    channel(channel &&) = default;
    channel & operator=(channel &&) = default;

    /// Close the sender side. Receiver will get nullopt after queue drains.
    void close_sender()
    {
        if (!sender_closed_.exchange(true, std::memory_order_acq_rel)) {
            // First time closing - shutdown the queue to wake up waiters
            // Note: This is fire-and-forget; queue destructor will do sync_wait
            (void)queue_.shutdown();
        }
    }

    /// Close the receiver side. Sender will throw disconnected{}.
    void close_receiver()
    {
        if (!receiver_closed_.exchange(true, std::memory_order_acq_rel)) {
            // First time closing - shutdown the queue to wake up waiters
            (void)queue_.shutdown();
        }
    }

    [[nodiscard]] bool sender_closed() const noexcept
    {
        return sender_closed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool receiver_closed() const noexcept
    {
        return receiver_closed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] coro::queue<T> & queue() noexcept
    {
        return queue_;
    }

private:
    coro::queue<T> queue_;
    std::atomic<bool> sender_closed_{false};
    std::atomic<bool> receiver_closed_{false};
};

template<typename T>
class subscription;

/// A handle for sending to a channel. Closes the sender side on destruction.
template<typename T>
class publication
{
public:
    publication(channel<T> & ch, scope & s)
        : channel_(&ch)
        , scope_(&s)
    {
    }

    ~publication()
    {
        if (channel_)
            channel_->close_sender();
    }

    // Non-copyable, moveable
    publication(const publication &) = delete;
    publication & operator=(const publication &) = delete;

    publication(publication && other) noexcept
        : channel_(other.channel_)
        , scope_(other.scope_)
    {
        other.channel_ = nullptr;
    }

    publication & operator=(publication && other) noexcept
    {
        if (this != &other) {
            if (channel_)
                channel_->close_sender();
            channel_ = other.channel_;
            scope_ = other.scope_;
            other.channel_ = nullptr;
        }
        return *this;
    }

    /// Push an item to the channel.
    /// Throws cancelled{} if scope is cancelled.
    /// Throws disconnected{} if receiver is gone.
    task<void> push(T item)
    {
        if (scope_->cancelled())
            throw cancelled{};

        if (channel_->receiver_closed())
            throw disconnected{};

        // Try to push - the queue will handle backpressure
        auto result = co_await channel_->queue().push(std::move(item));

        // Check if we were cancelled or disconnected during the push
        if (scope_->cancelled())
            throw cancelled{};

        if (result == coro::queue_produce_result::stopped) {
            // Queue was shut down - check why
            if (channel_->receiver_closed())
                throw disconnected{};
            throw cancelled{};
        }
    }

private:
    channel<T> * channel_;
    scope * scope_;
};

/// A handle for receiving from a channel. Closes the receiver side on destruction.
template<typename T>
class subscription
{
public:
    subscription(channel<T> & ch, scope & s)
        : channel_(&ch)
        , scope_(&s)
    {
    }

    ~subscription()
    {
        if (channel_)
            channel_->close_receiver();
    }

    // Non-copyable, moveable
    subscription(const subscription &) = delete;
    subscription & operator=(const subscription &) = delete;

    subscription(subscription && other) noexcept
        : channel_(other.channel_)
        , scope_(other.scope_)
    {
        other.channel_ = nullptr;
    }

    subscription & operator=(subscription && other) noexcept
    {
        if (this != &other) {
            if (channel_)
                channel_->close_receiver();
            channel_ = other.channel_;
            scope_ = other.scope_;
            other.channel_ = nullptr;
        }
        return *this;
    }

    /// Pop an item from the channel.
    /// Returns nullopt if sender closed and queue is empty.
    /// Throws cancelled{} if scope is cancelled.
    task<std::optional<T>> pop()
    {
        if (scope_->cancelled())
            throw cancelled{};

        auto result = co_await channel_->queue().pop();

        // Check if we were cancelled during the pop
        if (scope_->cancelled())
            throw cancelled{};

        if (result.has_value())
            co_return std::move(result.value());

        // Queue returned empty - sender must have closed
        co_return std::nullopt;
    }

private:
    channel<T> * channel_;
    scope * scope_;
};

/// Create a subscription to a channel bound to a scope.
template<typename T>
[[nodiscard]] auto subscribe(channel<T> & ch, scope & s)
{
    return subscription<T>(ch, s);
}

/// Create a publication to a channel bound to a scope.
template<typename T>
[[nodiscard]] auto publish(channel<T> & ch, scope & s)
{
    return publication<T>(ch, s);
}

} // namespace nxt
