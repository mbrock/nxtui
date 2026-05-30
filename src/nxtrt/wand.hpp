#pragma once

#include "nxtrt/wish.hpp"
#include "nxtrt/wish_ops.hpp"

namespace nxtrt {

class deck;
class wand;

namespace detail {
struct promise_base;
}

/// A suspended coroutine parked inside a wand.
///
/// Operation-specific wands store these records by coin. When the platform
/// completion arrives, `resume()` puts the task back onto the deck.
struct need
{
    void resume(deck & d) const;

    std::coroutine_handle<> handle;
    detail::promise_base * promise = nullptr;
};

template<typename T>
class urge;

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

/// Typed urge returned by a wand after preparing an operation.
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

/// Backend interface for staged platform/event-loop machinery.
///
/// `prepare()` is called synchronously while a coroutine is running. It can
/// allocate backend state, stage submission records, and return a typed urge.
/// The urge parks the coroutine at `await_suspend()`. After a deck round,
/// `wave()` lets the wand submit whatever it staged during that round.
class wand
{
public:
    virtual ~wand() = default;

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

    virtual void suspend(coin_t token, need task) = 0;
    virtual void cancel(coin_t token) = 0;
    virtual void wave(deck & d) = 0;

protected:
    virtual coin_t prep(
        deck & d,
        detail::promise_base & promise,
        detail::prepared_wish wish) = 0;
};

} // namespace nxtrt
