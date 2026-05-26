#pragma once

#include "nxtrt/task.hpp"

#include <coroutine>
#include <vector>

namespace nxtrt {

/// Manual-reset event for deck-local coordination.
///
/// `set()` wakes every task currently awaiting the event and makes future
/// awaits complete immediately until `reset()` is called.
class event
{
public:
    event() = default;

    event(const event &) = delete;
    event & operator=(const event &) = delete;
    event(event &&) = delete;
    event & operator=(event &&) = delete;

    ~event()
    {
        set();
    }

    [[nodiscard]] bool is_set() const noexcept
    {
        return set_;
    }

    void set()
    {
        set_ = true;
        wake_all();
    }

    void reset() noexcept
    {
        set_ = false;
    }

    class awaiter
    {
    public:
        explicit awaiter(event & owner) noexcept
            : owner_(&owner)
        {}

        [[nodiscard]] bool await_ready() const noexcept
        {
            return owner_ == nullptr || owner_->set_;
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
                    "nxtrt event awaited without a running deck"};

            owner_->waiters_.push_back(
                waiter{
                    .active_deck = active_deck,
                    .handle = awaiting,
                    .promise = running,
                });
        }

        void await_resume() const noexcept {}

    private:
        event * owner_ = nullptr;
    };

    [[nodiscard]] awaiter operator co_await() noexcept
    {
        return awaiter{*this};
    }

private:
    struct waiter
    {
        deck * active_deck = nullptr;
        std::coroutine_handle<> handle;
        detail::promise_base * promise = nullptr;
    };

    void wake_all()
    {
        auto waiters = std::vector<waiter>{};
        waiters.swap(waiters_);
        for (auto const & waiter : waiters) {
            if (waiter.active_deck != nullptr)
                waiter.active_deck->enqueue(waiter.handle, waiter.promise);
        }
    }

    bool set_ = false;
    std::vector<waiter> waiters_;
};

} // namespace nxtrt
