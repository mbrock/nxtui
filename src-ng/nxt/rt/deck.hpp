#pragma once

#include "nxt/rt/ids.hpp"

#include <concepts>
#include <coroutine>
#include <deque>
#include <functional>
#include <stdexcept>
#include <type_traits>

namespace nxt::rt {

template<typename T = void>
class task;
class deck;
struct yield_awaiter;

template<typename>
struct is_task : std::false_type
{};

template<typename T>
struct is_task<task<T>> : std::true_type
{};

template<typename T>
inline constexpr bool is_task_v = is_task<std::remove_cvref_t<T>>::value;

template<typename>
struct task_result;

template<typename T>
struct task_result<task<T>>
{
    using type = T;
};

template<typename T>
using task_result_t = typename task_result<std::remove_cvref_t<T>>::type;

template<typename Fn>
concept task_factory =
    std::invocable<Fn> && is_task_v<std::invoke_result_t<Fn>>;

namespace detail {

struct promise_base;

// Dynamic execution context for code currently being resumed by a deck.
//
// A C++ coroutine frame stores its own promise object, but ordinary functions
// called from inside the coroutine do not automatically receive that promise.
// These thread-local pointers are the minimal "ambient" hook that lets
// `co_await nxt::rt::yield()` and `co_await child_task` discover the currently
// running task while the deck pump is resuming it.
inline thread_local deck * current_deck = nullptr;
inline thread_local promise_base * current_promise = nullptr;

} // namespace detail

class deck
{
public:
    deck() = default;

    deck(const deck &) = delete;
    deck & operator=(const deck &) = delete;
    deck(deck &&) = delete;
    deck & operator=(deck &&) = delete;

    /// Return the id of the task currently being resumed by this deck.
    ///
    /// Outside `run_ready()` this returns the empty id. This is intentionally
    /// just ambient observation; durable task storage is a separate design
    /// question and does not live in this seed deck.
    [[nodiscard]] task_id current_task_id() const noexcept;

    /// True when no coroutine handles are queued for the pump.
    [[nodiscard]] bool empty() const noexcept
    {
        return ready_.empty();
    }

    /// Resume the handles that are ready at the start of this pump turn.
    ///
    /// This is one "pump" round. It first takes a snapshot of the current
    /// ready queue and leaves the deck empty. Any tasks enqueued by resumed
    /// work land in the next round's queue instead of mutating the collection
    /// currently being iterated.
    ///
    /// The deck does not own a thread or block for external I/O; hosts such as
    /// a terminal UI or Emacs module can decide when to call it.
    /// Re-entering it from a running task is rejected so one coroutine cannot
    /// unexpectedly resume sibling work in the middle of its own turn.
    void run_ready()
    {
        if (detail::current_promise != nullptr)
            throw std::runtime_error{"nxt::rt deck pump is not reentrant"};

        auto round = std::deque<ready_item>{};
        round.swap(ready_);

        for (auto const & item : round) {
            if (!item.handle || item.handle.done())
                continue;

            auto deck_guard = current_deck_guard{*this};
            auto promise_guard = current_promise_guard{item.promise};
            item.handle.resume();
        }
    }

    /// Keep pumping ready rounds until no work is queued.
    void run_until_idle()
    {
        while (!empty())
            run_ready();
    }

    template<typename T>
    void start(task<T> & t);

    /// Drive one root task until completion on this deck.
    ///
    /// The deadlock check catches the seed runtime's only current blocking
    /// condition: a task suspended but no future event/timer/fd machinery exists
    /// to enqueue it again.
    template<typename T>
    [[nodiscard]] T sync_wait(task<T> t)
    {
        start(t);
        while (!t.done()) {
            if (ready_.empty())
                throw std::runtime_error{"nxt::rt deck deadlock"};
            run_ready();
        }

        if constexpr (std::is_void_v<T>) {
            t.result();
        } else {
            return std::move(t).result();
        }
    }

    /// Create and drive a task from a nullary task factory.
    ///
    /// This is a tiny sender-like convenience: the callable is a lazy recipe
    /// that produces a fresh task when `sync_wait` starts it.
    template<task_factory Fn>
    [[nodiscard]] task_result_t<std::invoke_result_t<Fn>>
    sync_wait(Fn && fn)
    {
        return sync_wait(std::invoke(std::forward<Fn>(fn)));
    }

private:
    friend struct detail::promise_base;
    template<typename T>
    friend class task;
    friend struct yield_awaiter;

    struct ready_item
    {
        // The compiler/runtime handle used to resume the coroutine frame.
        std::coroutine_handle<> handle;
        // The promise belonging to `handle`, used only to restore ambient
        // current-task context while resuming.
        detail::promise_base * promise = nullptr;
    };

    /// Temporarily marks `d` as the deck currently resuming code.
    class current_deck_guard
    {
    public:
        explicit current_deck_guard(deck & d) noexcept
            : previous_(detail::current_deck)
        {
            detail::current_deck = &d;
        }

        ~current_deck_guard()
        {
            detail::current_deck = previous_;
        }

    private:
        deck * previous_;
    };

    /// Temporarily marks `promise` as the current coroutine promise.
    class current_promise_guard
    {
    public:
        explicit current_promise_guard(detail::promise_base * promise) noexcept
            : previous_(detail::current_promise)
        {
            detail::current_promise = promise;
        }

        ~current_promise_guard()
        {
            detail::current_promise = previous_;
        }

    private:
        detail::promise_base * previous_;
    };

    /// Put a coroutine handle on the ready queue for a later pump step.
    void enqueue(std::coroutine_handle<> handle, detail::promise_base * promise);

    std::deque<ready_item> ready_;
};

/// Awaitable that moves the current coroutine to the back of the active deck.
[[nodiscard]] yield_awaiter yield() noexcept;

} // namespace nxt::rt
