#pragma once

#include "nxt/rt/task.hpp"

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace nxt::rt {

template<typename T>
class [[nodiscard]] pipe;

namespace detail {

template<typename T>
struct pipe_promise final : promise_base
{
    using pipe_type = pipe<T>;
    using stored_type = std::remove_cv_t<T>;

    struct yield_awaitable
    {
        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<pipe_promise> coroutine) const
        {
            coroutine.promise().resume_continuation();
        }

        void await_resume() const noexcept {}
    };

    [[nodiscard]] pipe_type get_return_object() noexcept;

    template<typename Value>
        requires std::is_constructible_v<stored_type, Value &&>
    yield_awaitable yield_value(Value && value)
    {
        current_.emplace(std::forward<Value>(value));
        return {};
    }

    void return_void() noexcept {}

    void unhandled_exception() noexcept
    {
        exception_ = std::current_exception();
    }

    std::optional<stored_type> take_current()
    {
        if (exception_)
            std::rethrow_exception(exception_);

        auto value = std::move(current_);
        current_.reset();
        return value;
    }

private:
    std::optional<stored_type> current_;
    std::exception_ptr exception_;
};

} // namespace detail

template<typename T>
class [[nodiscard]] pipe
{
public:
    using promise_type = detail::pipe_promise<T>;
    using coroutine_handle = std::coroutine_handle<promise_type>;
    using value_type = std::remove_cv_t<T>;

    class next_awaiter
    {
    public:
        explicit next_awaiter(coroutine_handle coroutine) noexcept
            : coroutine_(coroutine)
        {}

        [[nodiscard]] bool await_ready() const noexcept
        {
            return !coroutine_ || coroutine_.done();
        }

        void await_suspend(std::coroutine_handle<> awaiting)
        {
            auto * active_deck = detail::current_deck;
            auto * awaiting_promise = detail::current_promise;
            if (active_deck == nullptr || awaiting_promise == nullptr)
                throw std::runtime_error{
                    "nxt::rt pipe awaited without a running deck"};

            auto & promise = coroutine_.promise();
            promise.set_continuation(awaiting, awaiting_promise);
            promise.enqueue_self(coroutine_);
        }

        std::optional<value_type> await_resume()
        {
            if (!coroutine_)
                return std::nullopt;
            return coroutine_.promise().take_current();
        }

    private:
        coroutine_handle coroutine_;
    };

    pipe() noexcept = default;

    explicit pipe(coroutine_handle coroutine) noexcept
        : coroutine_(coroutine)
    {}

    pipe(const pipe &) = delete;
    pipe & operator=(const pipe &) = delete;

    pipe(pipe && other) noexcept
        : coroutine_(std::exchange(other.coroutine_, nullptr))
    {}

    pipe & operator=(pipe && other) noexcept
    {
        if (this != &other) {
            destroy();
            coroutine_ = std::exchange(other.coroutine_, nullptr);
        }
        return *this;
    }

    ~pipe()
    {
        destroy();
    }

    [[nodiscard]] bool done() const noexcept
    {
        return !coroutine_ || coroutine_.done();
    }

    [[nodiscard]] task_id id() const noexcept
    {
        if (!coroutine_)
            return {};
        return coroutine_.promise().id;
    }

    [[nodiscard]] next_awaiter next() noexcept
    {
        return next_awaiter{coroutine_};
    }

private:
    bool destroy() noexcept
    {
        if (!coroutine_)
            return false;
        coroutine_.destroy();
        coroutine_ = nullptr;
        return true;
    }

    coroutine_handle coroutine_{nullptr};
};

namespace detail {

template<typename T>
inline auto pipe_promise<T>::get_return_object() noexcept -> pipe_type
{
    return pipe_type{
        std::coroutine_handle<pipe_promise<T>>::from_promise(*this)};
}

} // namespace detail

} // namespace nxt::rt
