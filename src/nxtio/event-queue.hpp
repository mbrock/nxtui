#pragma once

#include "nxtio/async-core.hpp"

#include <atomic>
#include <optional>
#include <stop_token>
#include <utility>

namespace nxt::io {

/// Small cancellable async event queue.
template<typename T>
class event_queue
{
public:
    /// Create an open queue.
    event_queue() = default;

    /// Request stop on destruction so pending producers/consumers can wake.
    ~event_queue()
    {
        stop_source_.request_stop();
    }

    event_queue(const event_queue &) = delete;
    event_queue & operator=(const event_queue &) = delete;
    event_queue(event_queue &&) = delete;
    event_queue & operator=(event_queue &&) = delete;

    /// Stop token associated with the queue.
    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        return stop_source_.get_token();
    }

    /// True when cancellation has been requested.
    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stop_source_.stop_requested();
    }

    /// Publish an item. Returns false once the queue is closed or stopped.
    nxt::task<bool> publish(T item)
    {
        if (closed_.load(std::memory_order_acquire)
            || stop_source_.stop_requested())
            co_return false;

        auto result = co_await queue_.push(std::move(item));
        co_return result == coro::queue_produce_result::produced;
    }

    /// Wait for the next item, or `std::nullopt` after shutdown.
    nxt::task<std::optional<T>> next()
    {
        auto result = co_await queue_.pop();
        if (result.has_value())
            co_return std::move(result.value());
        co_return std::nullopt;
    }

    /// Close the queue without requesting cancellation.
    nxt::task<> close()
    {
        closed_.store(true, std::memory_order_release);
        co_await queue_.shutdown();
    }

    /// Request cancellation and close the queue.
    nxt::task<> cancel()
    {
        stop_source_.request_stop();
        co_await close();
    }

private:
    nxt::queue<T> queue_;
    std::stop_source stop_source_;
    std::atomic<bool> closed_{false};
};

} // namespace nxt::io
