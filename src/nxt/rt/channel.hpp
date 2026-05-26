#pragma once

#include "nxt/rt/task.hpp"

#include <coroutine>
#include <deque>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxt::rt {

/// Buffered deck-local channel.
///
/// This is intentionally small: it is for runtime/app coordination inside a
/// single `deck`, not a cross-thread queue. Values are buffered until consumed;
/// `close()` rejects future sends and lets consumers drain the buffer before
/// returning `std::nullopt`.
template<typename T>
class channel
{
public:
    using value_type = std::remove_cv_t<T>;

    channel() = default;

    channel(const channel &) = delete;
    channel & operator=(const channel &) = delete;
    channel(channel &&) = delete;
    channel & operator=(channel &&) = delete;

    ~channel()
    {
        cancel();
    }

    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        return stop_.get_token();
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stop_.stop_requested();
    }

    [[nodiscard]] bool closed() const noexcept
    {
        return closed_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return values_.empty();
    }

    [[nodiscard]] std::optional<value_type> try_pop()
    {
        if (values_.empty())
            return std::nullopt;

        auto value = std::move(values_.front());
        values_.pop_front();
        return value;
    }

    [[nodiscard]] bool try_publish(T value)
    {
        if (closed_ || stop_.stop_requested())
            return false;

        if (!waiters_.empty()) {
            auto waiter = waiters_.front();
            waiters_.pop_front();
            waiter.awaiter->set_value(std::move(value));
            waiter.resume();
            return true;
        }

        values_.push_back(std::move(value));
        return true;
    }

    [[nodiscard]] task<bool> publish(T value)
    {
        co_return try_publish(std::move(value));
    }

    [[nodiscard]] task<bool> push(T value)
    {
        co_return try_publish(std::move(value));
    }

    void close()
    {
        if (closed_)
            return;
        closed_ = true;
        wake_all_empty();
    }

    void cancel()
    {
        stop_.request_stop();
        close();
    }

    class next_awaiter
    {
    public:
        explicit next_awaiter(channel & owner) noexcept
            : owner_(&owner)
        {}

        [[nodiscard]] bool await_ready()
        {
            if (owner_ == nullptr) {
                ready_ = true;
                return true;
            }

            value_ = owner_->try_pop();
            ready_ = value_.has_value()
                || owner_->closed_
                || owner_->stop_.stop_requested();
            return ready_;
        }

        void await_suspend(std::coroutine_handle<> awaiting)
        {
            auto * current = detail::current_env;
            auto * active_deck =
                current == nullptr ? nullptr : current->current_deck;
            auto * running =
                current == nullptr ? nullptr : current->current_promise;
            if (active_deck == nullptr || running == nullptr)
                throw runtime_error{
                    "nxt::rt channel awaited without a running deck"};

            if (owner_->closed_ || owner_->stop_.stop_requested()) {
                active_deck->enqueue(awaiting, running);
                return;
            }

            if (auto value = owner_->try_pop()) {
                value_ = std::move(value);
                active_deck->enqueue(awaiting, running);
                return;
            }

            owner_->waiters_.push_back(
                waiter{
                    .awaiter = this,
                    .active_deck = active_deck,
                    .handle = awaiting,
                    .promise = running,
                });
        }

        std::optional<value_type> await_resume()
        {
            return std::move(value_);
        }

    private:
        friend class channel;

        void set_value(T value)
        {
            value_.emplace(std::move(value));
        }

        channel * owner_ = nullptr;
        std::optional<value_type> value_;
        bool ready_ = false;
    };

    [[nodiscard]] next_awaiter next() noexcept
    {
        return next_awaiter{*this};
    }

private:
    struct waiter
    {
        next_awaiter * awaiter = nullptr;
        deck * active_deck = nullptr;
        std::coroutine_handle<> handle;
        detail::promise_base * promise = nullptr;

        void resume() const
        {
            if (active_deck != nullptr)
                active_deck->enqueue(handle, promise);
        }
    };

    void wake_all_empty()
    {
        auto waiters = std::deque<waiter>{};
        waiters.swap(waiters_);
        for (auto const & waiter : waiters)
            waiter.resume();
    }

    std::deque<value_type> values_;
    std::deque<waiter> waiters_;
    std::stop_source stop_;
    bool closed_ = false;
};

} // namespace nxt::rt
