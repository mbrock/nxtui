#pragma once

#include "nxtrt/ids.hpp"
#include "nxtrt/env.hpp"
#include "nxtrt/trace.hpp"
#include "nxtrt/wish.hpp"

#include <concepts>
#include <coroutine>
#include <deque>
#include <functional>
#include <string>
#include <type_traits>

namespace nxtrt {

template<typename T = void>
class task;
class firm;
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

template<typename Fn>
concept stored_task_factory =
    std::invocable<Fn &> && is_task_v<std::invoke_result_t<Fn &>>;

template<typename Fn>
using stored_task_result_t = task_result_t<std::invoke_result_t<Fn &>>;

namespace detail {

struct promise_base;

} // namespace detail

class deck
{
public:
    deck() = default;
    explicit deck(wand * w) noexcept
        : wand_(w)
    {}

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

    /// Set the backend used by closed wish operations resumed by this deck.
    void set_wand(wand * w) noexcept
    {
        wand_ = w;
    }

    /// Return the backend currently attached to this deck, if any.
    [[nodiscard]] wand * current_wand() const noexcept
    {
        return wand_;
    }

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
        dump_if_requested();
        run_ready_with();
        if (wand_ != nullptr) {
            trace("deck wave wand");
            wand_->wave(*this);
        }
        dump_if_requested();
    }

    /// Pump one ready round with `w` as the active backend, then wave it.
    ///
    /// Awaiting a closed wish operation synchronously asks the active wand to
    /// prepare a typed waiter. Once the round ends, `wave()` lets the wand
    /// submit whatever platform work it staged during coroutine execution.
    void run_ready(wand & w)
    {
        auto guard = wand_swap{*this, &w};
        run_ready();
    }

    /// Keep pumping ready rounds until no work is queued.
    void run_until_idle()
    {
        while (!empty())
            run_ready();
    }

    /// Keep pumping currently ready work, waving the wand after each round.
    ///
    /// This still does not block for platform events. If waving the wand
    /// immediately fulfills something, the next loop iteration will see that
    /// newly-ready task.
    void run_until_idle(wand & w)
    {
        while (!empty())
            run_ready(w);
    }

private:
    struct wand_swap
    {
        deck & d;
        wand * previous = nullptr;

        wand_swap(deck & d, wand * next) noexcept
            : d(d)
            , previous(d.wand_)
        {
            d.wand_ = next;
        }

        wand_swap(const wand_swap &) = delete;
        wand_swap & operator=(const wand_swap &) = delete;

        ~wand_swap()
        {
            d.wand_ = previous;
        }
    };

    void run_ready_with()
    {
        auto * env = current_env();
        if (env != nullptr && env->current_promise != nullptr)
            throw runtime_error{"nxtrt deck pump is not reentrant"};

        auto round = std::deque<ready_item>{};
        round.swap(ready_);
        trace("deck round begin size={}", round.size());

        for (auto const & item : round)
            item.resume_if_ready(*this);
        trace("deck round end ready={}", ready_.size());
    }

    void dump_if_requested();

public:
    [[nodiscard]] std::string runtime_dump_text() const;

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
                throw runtime_error{"nxtrt deck deadlock"};
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
    /// @cond
    friend struct detail::promise_base;
    template<typename T>
    friend class task;
    friend class firm;
    friend struct parked_task;
    friend struct yield_awaiter;
    /// @endcond

    struct ready_item
    {
        /// Resume this coroutine frame if it still has work to do.
        ///
        /// The deck context is established once per pump round by
        /// `run_ready()`. Each item only needs to restore its own promise as
        /// the ambient current task before transferring control to the
        /// compiler/runtime handle.
        void resume_if_ready(deck & d) const;

        // The compiler/runtime handle used to resume the coroutine frame.
        std::coroutine_handle<> handle;
        // The promise belonging to `handle`, used only to restore ambient
        // current-task context while resuming.
        detail::promise_base * promise = nullptr;
    };

    /// Put a coroutine handle on the ready queue for a later pump step.
    void enqueue(std::coroutine_handle<> handle, detail::promise_base * promise);

    std::deque<ready_item> ready_;
    wand * wand_ = nullptr;
};

/// Awaitable that moves the current coroutine to the back of the active deck.
[[nodiscard]] yield_awaiter yield() noexcept;

} // namespace nxtrt
