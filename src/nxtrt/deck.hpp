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
class deck;
struct yield_awaiter;

/// True for `task<T>`; see `is_task_v`.
template<typename>
struct is_task : std::false_type
{};

template<typename T>
struct is_task<task<T>> : std::true_type
{};

/// True if `T`, ignoring cv and references, is some `task<U>`.
template<typename T>
inline constexpr bool is_task_v = is_task<std::remove_cvref_t<T>>::value;

/// The value type `U` of a `task<U>`; see `task_result_t`.
template<typename>
struct task_result;

template<typename T>
struct task_result<task<T>>
{
    using type = T;
};

/// The value type of a task type: `task_result_t<task<int>&>` is `int`.
template<typename T>
using task_result_t = typename task_result<std::remove_cvref_t<T>>::type;

/// A callable that, given `Args...`, returns a `task` by value.
template<typename Fn, typename... Args>
concept task_factory = std::invocable<Fn, Args...>
                       && is_task_v<std::invoke_result_t<Fn, Args...>>;

/// A `task_factory` that is invoked as a stored, mutable lvalue, which is
/// how `deck::sync_wait`, `with_env` and `finally` call their factories.
template<typename Fn, typename... Args>
concept stored_task_factory = task_factory<Fn &, Args...>;

/// The task value type produced by a `stored_task_factory`.
template<typename Fn>
using stored_task_result_t = task_result_t<std::invoke_result_t<Fn &>>;

namespace detail {

struct promise_base;

} // namespace detail

/// Whether a row of a deck's task table holds a task.
enum class deck_task_state : std::uint8_t
{
    vacant,
    live,
};

/// One row of a deck's task table: the registered task's id, handle and
/// promise. Callers only allocate these, through
/// `static_deck_task_storage` or a span passed to `deck_task_storage_ref`;
/// the deck manages their contents.
struct deck_task_record
{
    task_id id;
    std::coroutine_handle<> handle;
    detail::promise_base * promise = nullptr;
    deck_task_state state = deck_task_state::vacant;
    std::uint8_t era = 1;
};

/// Borrowed task-table storage for a @ref nxtrt::deck "deck": a span of
/// value-initialized `deck_task_record`s. The records must outlive the deck
/// and must not be shared with another deck. The span's size is the deck's
/// task capacity.
struct deck_task_storage_ref
{
    deck_task_storage_ref() = default;

    explicit deck_task_storage_ref(std::span<deck_task_record> records)
        : records(records)
    {}

    std::span<deck_task_record> records;
};

/// Inline storage for a deck task table of `N` rows, so a deck can run
/// without heap-allocating its registry.
///
/// Each task the deck has queued at least once holds one row until its
/// frame is destroyed (finished tasks keep their row while their `task`
/// handle lives). When all rows are taken, queuing another task throws
/// `runtime_error` ("nxtrt deck task table is full"). Keep the storage
/// alive, and in place, for as long as the deck that uses it.
///
/// @code
/// auto storage = nxtrt::static_deck_task_storage<16>{};
/// auto deck = nxtrt::deck{storage};
/// @endcode
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

/// A single-threaded cooperative scheduler for tasks: a ready queue plus a
/// table of registered tasks.
///
/// A deck owns no thread and never blocks. The host calls `run_ready()` to
/// resume every task that was ready at the start of the call, each until
/// its next suspension, and then to wave the attached @ref nxtrt::wand
/// "wand" so it can submit the I/O those tasks asked for. Waiting for
/// platform events is the wand's job (for example a wand's
/// `run_until_done`); `sync_wait` drives a deck for work that needs no
/// waiting, or whose wand completes everything during `wave()`.
///
/// Every task the deck resumes is registered in its task table and gets a
/// @ref nxtrt::task_id "task_id"; the queue holds ids, so a task destroyed
/// while queued is skipped. The table has a fixed capacity: 4096 rows by
/// default (heap-allocated), or the caller's storage
/// (`static_deck_task_storage`, `deck_task_storage_ref`).
///
/// A deck, its tasks and its wand are confined to one thread. The deck is
/// neither copyable nor movable, and its pump is not reentrant: calling
/// `run_ready()` from inside a running task throws. See @ref rt_deck.
///
/// @code
/// auto deck = nxtrt::deck{};
/// int n = deck.sync_wait([] { return compute(); });
/// @endcode
class deck
{
public:
    /// Task-table rows allocated by the constructors without storage.
    static constexpr std::size_t default_task_capacity = 4096;

    /// A deck with a heap-allocated table of `default_task_capacity` rows
    /// and no wand. Wishes cannot be awaited until one is attached.
    deck()
        : owned_tasks_(default_task_capacity)
        , tasks_(owned_tasks_)
    {}

    /// A deck with the default table and `w` as its wand (borrowed; it must
    /// outlive the deck's use of it).
    explicit deck(wand * w)
        : deck()
    {
        wand_ = w;
    }

    /// A deck whose task table is the borrowed `storage`, optionally with a
    /// wand. Nothing is allocated for the table.
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

    /// The id of the task this deck is currently resuming, or the empty id
    /// when called outside one of this deck's tasks.
    [[nodiscard]] task_id current_task_id() const noexcept;

    /// Attach the wand (backend) that wishes awaited on this deck use. The
    /// wand is borrowed. Do not switch wands while wishes are parked on the
    /// old one.
    void set_wand(wand * w) noexcept
    {
        wand_ = w;
    }

    /// Return the backend currently attached to this deck, if any.
    [[nodiscard]] wand * current_wand() const noexcept
    {
        return wand_;
    }

    /// True when no task ids are queued. Tasks parked on wishes do not
    /// count, so a deck can be empty while work is still outstanding.
    [[nodiscard]] bool empty() const noexcept
    {
        return ready_.empty();
    }

    /// Run one round: resume each task that was queued when the call
    /// started, in queue order, then call `wave()` on the attached wand.
    ///
    /// Tasks queued during the round (continuations, yields, newly started
    /// children) wait for the next round. Ids whose task was destroyed or
    /// already finished are skipped. A task's own exceptions are stored in
    /// its result, not thrown here. Throws `runtime_error` if called from
    /// inside a running task.
    ///
    /// Never blocks on I/O by itself, so a host such as a UI event loop can
    /// call it whenever it likes.
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

    /// Run one round with `w` temporarily attached as the wand, then wave
    /// `w`. The previous wand is restored afterwards.
    void run_ready(wand & w)
    {
        auto guard = wand_swap{*this, &w};
        run_ready();
    }

    /// Run rounds until the ready queue is empty. Does not wait for tasks
    /// parked on wishes; those are resumed only once their wand completes
    /// them and the deck is pumped again.
    void run_until_idle()
    {
        while (!empty())
            run_ready();
    }

    /// `run_until_idle()` with `w` as the wand for each round. Still does
    /// not block for platform events; work that `w` completes during
    /// `wave()` is picked up by the next round.
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
    /// A human-readable dump of parked tasks and the ready queue, for
    /// debugging stuck programs.
    [[nodiscard]] std::string runtime_dump_text() const;

    /// Queue `t` to run on this deck, without awaiting it. `t` stays owned
    /// by the caller, which must keep it alive until it is done (or destroy
    /// it, which removes it). Does nothing for an empty or finished task.
    /// Throws `runtime_error` if the task table is full or `t` is
    /// registered with another deck.
    template<typename T>
    void start(task<T> & t);

    /// Rejects an already-created task at compile time.
    ///
    /// A task captures the runtime environment when it is created, and the
    /// root environment only exists inside `sync_wait`, so the root must be
    /// created there: pass the factory instead.
    template<typename T>
    void sync_wait(task<T>)
    {
        static_assert(
            sizeof(T *) == 0,
            "deck::sync_wait takes a task factory, not a task: write "
            "d.sync_wait(make_task) or d.sync_wait([&] { return "
            "make_task(args); }) so the root task is created inside its runtime");
    }

    /// Create a root task by calling `fn(args...)` in a fresh root
    /// environment, run this deck until it completes, and return its value
    /// (or rethrow its exception).
    ///
    /// `fn` is moved into storage that outlives the task, so it may be a
    /// capturing coroutine lambda. Runs rounds with `run_ready()`; if the
    /// root is unfinished and nothing is queued, throws `runtime_error`
    /// ("nxtrt deck deadlock") with a runtime dump. So `sync_wait` suits
    /// work that needs no waiting on platform events, or a wand that
    /// completes everything during `wave()`; real I/O wands have their own
    /// drivers. Blocks the calling thread until done; do not call it from a
    /// task running on the same deck.
    template<typename Fn, typename... Args>
        requires task_factory<std::decay_t<Fn> &, Args...>
    [[nodiscard]] task_result_t<
        std::invoke_result_t<std::decay_t<Fn> &, Args...>>
    sync_wait(Fn && fn, Args &&... args);

private:
    /// Drive one root task, created in its root environment, until
    /// completion. Throws a deadlock error when the root is unfinished and
    /// nothing is queued, since the deck has no way to wait for the wand.
    template<typename T>
    [[nodiscard]] T drive(task<T> t)
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

    /// @cond
    friend struct detail::promise_base;
    template<typename T>
    friend class task;
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

/// `co_await nxtrt::yield()` suspends the running task and queues it again
/// at the back of its deck's ready queue, so it resumes in the next round,
/// after the other ready tasks. It does not check for stop. Throws
/// `runtime_error` outside a running deck task.
[[nodiscard]] yield_awaiter yield() noexcept;

} // namespace nxtrt
