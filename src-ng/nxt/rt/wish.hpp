#pragma once

#include <coroutine>
#include <cstdint>

namespace nxt::rt {

class deck;
class wand;

namespace detail {
struct promise_base;
}

using wait_token = std::uint64_t;

/// A suspended coroutine parked inside a wand.
///
/// Operation-specific wands store these records by token. When the platform
/// completion arrives, `resume()` puts the task back onto the deck.
struct parked_task
{
    void resume(deck & d) const;

    std::coroutine_handle<> handle;
    detail::promise_base * promise = nullptr;
};

template<typename T>
class waiter;

/// Void-result waiter returned by a wand after preparing an operation.
///
/// For now the runtime only has `manual_wish`, whose result type is `void`.
/// Non-void waiters can later own or reference a typed result slot and return
/// that value from `await_resume()`.
template<>
class waiter<void>
{
public:
    waiter() = default;
    waiter(wand & source, wait_token token) noexcept;

    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> awaiting) const;
    void await_resume() const noexcept {}

private:
    wand * source_ = nullptr;
    wait_token token_ = 0;
};

/// Closed operation type for deterministic/manual tests.
///
/// This is deliberately more like a tiny SQE recipe than a generic variant:
/// the operation owns its input parameters and names its result type.
struct manual_wish
{
    using result_type = void;

    wait_token token = 0;

    waiter<void> operator co_await() const;
};

/// Backend interface for staged platform/event-loop machinery.
///
/// `prepare()` is called synchronously while a coroutine is running. It can
/// allocate backend state, stage submission records, and return a typed waiter.
/// The waiter parks the coroutine at `await_suspend()`. After a deck round,
/// `wave()` lets the wand submit whatever it staged during that round.
class wand
{
public:
    virtual ~wand() = default;

    virtual waiter<void> prepare(
        deck & d,
        detail::promise_base & promise,
        manual_wish wish) = 0;

    virtual void suspend(wait_token token, parked_task task) = 0;
    virtual void wave(deck & d) = 0;
};

} // namespace nxt::rt
