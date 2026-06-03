#pragma once

#include "nxtrt/ids.hpp"
#include "nxtrt/env.hpp"
#include "nxtrt/trace.hpp"
#include "nxtrt/wand.hpp"

#include <array>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

enum class deck_task_state : std::uint8_t
{
    vacant,
    live,
};

struct deck_task_record
{
    task_id id;
    std::coroutine_handle<> handle;
    detail::promise_base * promise = nullptr;
    deck_task_state state = deck_task_state::vacant;
    std::uint8_t era = 1;
};

struct deck_task_storage_ref
{
    deck_task_storage_ref() = default;

    explicit deck_task_storage_ref(std::span<deck_task_record> records)
        : records(records)
    {}

    std::span<deck_task_record> records;
};

template<std::size_t N>
class static_deck_task_storage
{
public:
    [[nodiscard]] deck_task_storage_ref ref() noexcept
    {
        return deck_task_storage_ref{std::span{records_}};
    }

    [[nodiscard]] operator deck_task_storage_ref() noexcept
    {
        return ref();
    }

private:
    std::array<deck_task_record, N> records_{};
};

class deck
{
public:
    static constexpr std::size_t default_task_capacity = 4096;

    deck()
        : owned_tasks_(default_task_capacity)
        , tasks_(owned_tasks_)
    {}

    explicit deck(wand * w)
        : deck()
    {
        wand_ = w;
    }

    explicit deck(deck_task_storage_ref storage, wand * w = nullptr) noexcept
        : tasks_(storage.records)
        , wand_(w)
    {}

    deck(wand * w, deck_task_storage_ref storage) noexcept
        : deck(storage, w)
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

    /// True when no task ids are queued for the pump.
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
    /// prepare a typed urge. Once the round ends, `wave()` lets the wand
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

        auto round = std::deque<task_id>{};
        round.swap(ready_);
        trace("deck round begin size={}", round.size());

        for (auto id : round)
            resume_if_ready(id);
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
            if (ready_.empty()) {
                auto message = std::string{"nxtrt deck deadlock\n"};
                message += runtime_dump_text();
                throw runtime_error{std::move(message)};
            }
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
    sync_wait(Fn && fn);

private:
    /// @cond
    friend struct detail::promise_base;
    template<typename T>
    friend class task;
    friend class firm;
    friend struct need;
    friend struct yield_awaiter;
    /// @endcond

    /// Put a coroutine handle on the ready queue for a later pump step.
    task_id enqueue(
        std::coroutine_handle<> handle,
        detail::promise_base * promise);

    [[nodiscard]] task_id register_task(
        std::coroutine_handle<> handle,
        detail::promise_base * promise);
    void unregister_task(task_id id, detail::promise_base * promise) noexcept;
    [[nodiscard]] deck_task_record * resolve(task_id id) noexcept;
    [[nodiscard]] const deck_task_record * resolve(task_id id) const noexcept;
    void resume_if_ready(task_id id);

    std::vector<deck_task_record> owned_tasks_;
    std::span<deck_task_record> tasks_;
    std::deque<task_id> ready_;
    wand * wand_ = nullptr;
};

/// Awaitable that moves the current coroutine to the back of the active deck.
[[nodiscard]] yield_awaiter yield() noexcept;

} // namespace nxtrt
