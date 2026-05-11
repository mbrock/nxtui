#pragma once

#include <exception>
#include <functional>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

#include <coro/when_any.hpp>

#include "nxtio/async-core.hpp"

namespace nxt {

struct empty_context
{};

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
/// A scope owns a stop source and links it to an optional parent stop
/// token. It represents a bounded lifetime for work spawned inside a
/// process.
template<typename Context = empty_context>
class scope
{
public:
    explicit scope(nxt::scheduler & sched, Context context = {})
        : sched_(sched)
        , context_(std::move(context))
    {
    }

    /// Create a scope with a parent stop token.
    /// When the parent is cancelled, this scope is also cancelled.
    scope(
        nxt::scheduler & sched,
        std::stop_token parent_token,
        Context context = {})
        : sched_(sched)
        , context_(std::move(context))
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

    /// Create a child scope with the same context.
    [[nodiscard]] scope subscope()
    {
        return scope{sched_, stop_token(), context_};
    }

    /// Create a child scope with a replacement context.
    template<typename ChildContext>
    [[nodiscard]] scope<ChildContext> subscope(ChildContext context)
    {
        return scope<ChildContext>{
            sched_,
            stop_token(),
            std::move(context)};
    }

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
    [[nodiscard]] nxt::scheduler & scheduler() noexcept
    {
        return sched_;
    }

    /// Access the contextual capabilities carried by this scope.
    [[nodiscard]] Context & context() noexcept
    {
        return context_;
    }

    [[nodiscard]] const Context & context() const noexcept
    {
        return context_;
    }

    /// Throw cancelled{} if the scope has been cancelled.
    void check() const
    {
        if (cancelled())
            throw nxt::cancelled{};
    }

    /// Add a void task to this scope. It starts when `all()` or `any()`
    /// is awaited.
    void spawn(task<> t)
    {
        tasks_.push_back(std::move(t));
    }

    /// Run all awaitables to completion within this scope.
    template<typename... Awaitables>
    [[nodiscard]] auto all(Awaitables &&... awaitables)
    {
        return coro::when_all(std::forward<Awaitables>(awaitables)...);
    }

    /// Run stored void tasks to completion within this scope.
    [[nodiscard]] auto all(std::vector<task<>> tasks)
    {
        return coro::when_all(std::move(tasks));
    }

    /// Run all tasks previously spawned into this scope.
    [[nodiscard]] auto all()
    {
        return all(std::move(tasks_));
    }

    /// Run awaitables until one completes, then request cancellation of
    /// this scope. Awaitables that should stop early must observe this
    /// scope's stop token.
    template<typename... Awaitables>
    [[nodiscard]] auto any(Awaitables &&... awaitables)
    {
        return coro::when_any(
            stop_source_, std::forward<Awaitables>(awaitables)...);
    }

    /// Run stored void tasks until one completes, then request
    /// cancellation of this scope.
    [[nodiscard]] auto any(std::vector<task<>> tasks)
    {
        return coro::when_any(stop_source_, std::move(tasks));
    }

    /// Run previously spawned tasks until one completes, then request
    /// cancellation of this scope.
    [[nodiscard]] auto any()
    {
        return any(std::move(tasks_));
    }

private:
    nxt::scheduler & sched_;
    std::stop_source stop_source_;
    Context context_;
    std::optional<std::stop_callback<std::function<void()>>> parent_callback_;
    std::vector<task<>> tasks_;
};

} // namespace nxt
