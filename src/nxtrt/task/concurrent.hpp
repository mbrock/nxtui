#pragma once

// Groups: run a fixed set or a range of tasks concurrently. Predicates
// decide when the rest are cancelled; every started job settles before
// return. Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/compose.hpp"

#include <expected>
#include <vector>

namespace nxtrt {

/// How one job of a group settled: its value, or the exception it ended with.
///
/// A job that was stopped before it ever started settles as an
/// `operation_cancelled` error. A job that was stopped after starting
/// settles however it ended: usually `operation_cancelled`, but a job that
/// ignores the stop can still produce a value. Test for cancellation with
/// `is_operation_cancelled(o.error())`.
template<typename T>
using outcome = std::expected<T, std::exception_ptr>;

/// @name Group policies
/// A group policy is a `noexcept` callable `bool(std::size_t index, bool
/// failed)`. The group calls it at each job's final suspension, with the
/// job's position and whether it ended with an exception, until it first
/// returns true; then the group requests stop on every other unfinished
/// job, and jobs that have not started yet are never started. Any lambda
/// with that signature works as a policy.
/// @{

/// Let every job run to completion; never stop the others.
struct all_group
{
    bool operator()(std::size_t, bool) const noexcept
    {
        return false;
    }
};

/// Stop the others when a job fails.
struct fail_fast_group
{
    bool operator()(std::size_t, bool failed) const noexcept
    {
        return failed;
    }
};

/// Stop the others when a job succeeds.
struct first_success_group
{
    bool operator()(std::size_t, bool failed) const noexcept
    {
        return !failed;
    }
};

/// Stop the others when any job settles.
struct first_completion_group
{
    bool operator()(std::size_t, bool) const noexcept
    {
        return true;
    }
};

/// Stop the companions when the first job (the primary) settles, whether it
/// succeeded or failed. Companions settling, even by failing, stop nothing.
///
/// Use this when a sibling (a watcher, a heartbeat) should live exactly as
/// long as the main work:
/// `settle(std::tuple{main(), watcher()}, primary_group{})`.
struct primary_group
{
    bool operator()(std::size_t index, bool) const noexcept
    {
        return index == 0;
    }
};

/// @}

namespace detail {

template<typename Policy>
concept group_policy =
    std::is_nothrow_invocable_r_v<bool, Policy &, std::size_t, bool>;

template<typename T>
[[nodiscard]] outcome<T> cancelled_outcome()
{
    return outcome<T>{
        std::unexpected{std::make_exception_ptr(operation_cancelled{})}};
}

/// Single-deck countdown. Reaching zero queues the waiter, never resumes it
/// inline: the completing child's final suspension must finish first.
struct countdown
{
    deck & executor;
    std::size_t pending = 0;
    need waiter{};

    void arrive() noexcept
    {
        if (--pending == 0 && waiter.handle)
            std::exchange(waiter, {}).resume(executor);
    }

    bool await_ready() const noexcept
    {
        return pending == 0;
    }

    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        waiter = need{handle, current_env->current_promise};
    }

    void await_resume() const noexcept {}
};

/// Visit tuple positions with compile-time indices, or range positions with
/// ordinary indices. The same setup and stop logic works for both shapes.
template<typename Tasks, typename Visit>
void visit_tasks(Tasks & tasks, Visit visit)
{
    if constexpr (std::ranges::range<Tasks>) {
        auto index = std::size_t{0};
        for (auto & child : tasks)
            visit(child, index++);
    } else {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (visit(
                 std::get<Is>(tasks),
                 std::integral_constant<std::size_t, Is>{}),
             ...);
        }(std::make_index_sequence<std::tuple_size_v<Tasks>>{});
    }
}

/// Prepare links before scheduling anything, then connect and start in
/// input order. Link callbacks inspect promises without moving their
/// results.
template<typename Tasks, group_policy Policy>
task<void> run_group(Tasks & tasks, Policy policy)
{
    auto * env = current_env;
    if (!env || !env->current_deck || !env->current_promise)
        throw runtime_error{"nxtrt group used without a running deck"};
    visit_tasks(tasks, [](auto & child, auto) {
        if (!child.handle())
            throw runtime_error{"nxtrt group received an empty task"};
    });

    auto done = countdown{*env->current_deck};
    auto stopping = false;
    auto parent_stopped = false;
    auto setup_failure = std::exception_ptr{};
    auto stop = [&]() noexcept {
        stopping = true;
        visit_tasks(tasks, [](auto & child, auto) {
            if (child.id() && !child.done())
                child.request_stop();
        });
    };
    auto make_link = [&](auto & child, std::size_t index) {
        return completion_link{[&, index]() noexcept {
            auto failed = false;
            try {
                (void) child.result();
            } catch (...) {
                failed = true;
            }
            if (!stopping && std::invoke(policy, index, failed))
                stop();
            done.arrive();
        }};
    };
    // All callback storage exists before a child starts, so partial setup
    // failure cannot unwind registrations that still need to count down.
    auto registrations = [&] {
        if constexpr (std::ranges::range<Tasks>) {
            using link_type = decltype(make_link(tasks[0], 0));
            auto out = std::vector<link_type>{};
            out.reserve(tasks.size());
            visit_tasks(tasks, [&](auto & child, auto index) {
                out.push_back(make_link(child, index));
            });
            return out;
        } else {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return std::tuple{make_link(std::get<Is>(tasks), Is)...};
            }(std::make_index_sequence<std::tuple_size_v<Tasks>>{});
        }
    }();
    auto parent_stop = std::stop_callback{
        env->current_promise->stop_token(), [&]() noexcept {
            parent_stopped = true;
            stop();
        }};
    try {
        visit_tasks(tasks, [&](auto & child, auto index) {
            if (!child.done() && stopping)
                return;
            auto & link = [&]() -> auto & {
                if constexpr (std::ranges::range<Tasks>)
                    return registrations[index];
                else
                    return std::get<decltype(index)::value>(registrations);
            }();
            if (child.done()) {
                ++done.pending;
                link.connect(
                    child); // Inline arrival; never schedule again.
            } else {
                child.handle().promise().env.copy_entries_from(*env);
                link.connect(child);
                env->current_deck->start(child);
                ++done.pending; // Count only successfully queued children.
            }
        });
    } catch (...) {
        setup_failure = std::current_exception();
        stop();
    }
    // This await is deliberately not cancellable: stop requests drain work,
    // rather than abandoning frames still referenced by children.
    co_await done;
    if (setup_failure)
        rethrow(setup_failure);
    if (parent_stopped)
        throw operation_cancelled{};
}

template<typename T>
outcome<T> extract_outcome(task<T> & child)
{
    if (!child.done())
        return cancelled_outcome<T>();
    try {
        if constexpr (std::is_void_v<T>) {
            child.handle().promise().result();
            return outcome<T>{std::in_place};
        } else {
            return outcome<T>{
                std::in_place, std::move(child.handle().promise()).result()};
        }
    } catch (...) {
        return outcome<T>{std::unexpected{std::current_exception()}};
    }
}

template<typename T>
[[nodiscard]] auto take_outcome(outcome<T> value)
{
    if (!value)
        rethrow(value.error());
    if constexpr (std::is_void_v<T>)
        return std::monostate{};
    else
        return std::move(*value);
}

template<typename Tuple, std::size_t... Is>
[[nodiscard]] auto
take_all_or_throw(Tuple & outcomes, std::index_sequence<Is...>)
{
    return std::tuple{
        take_outcome(std::move(std::get<Is>(outcomes)))...,
    };
}

/// Remember the completion that triggers stop. The group stops consulting
/// the policy once it returns true, so draining cannot replace this index.
template<group_policy Policy>
struct record_first
{
    std::optional<std::size_t> & first;
    Policy policy{};

    bool operator()(std::size_t index, bool failed) const noexcept
    {
        if (!std::invoke(policy, index, failed))
            return false;
        first = index;
        return true;
    }
};

/// Rethrow the error of the job that triggered the stop, so a sibling's
/// `operation_cancelled` caused by that stop never masks the real failure.
template<typename Tuple, std::size_t... Is>
void rethrow_first_failure(
    Tuple & outcomes, std::size_t first, std::index_sequence<Is...>)
{
    ((Is == first ? rethrow(std::get<Is>(outcomes).error()) : void()), ...);
}

} // namespace detail

/// Run a fixed set of tasks concurrently on the current deck and wait until
/// every started one has finished. Returns each job's
/// @ref nxtrt::outcome "outcome" in tuple order.
///
/// The group takes ownership of the tasks. When the returned task runs, it
/// starts the jobs in tuple order on its own deck (jobs see the group's
/// env). Tasks that are already done count as settled immediately and are
/// not run again. The policy `execution` (see the group policies above)
/// decides when to stop the remaining jobs; a job stopped before it started
/// settles as `operation_cancelled`. Job failures never make `settle`
/// throw; they are reported in the outcomes.
///
/// Draining: `settle` never returns while a started job is still running,
/// even when stopped, so jobs may safely borrow from the caller's frame.
///
/// Cancellation: stopping the task that awaits `settle` stops every job;
/// once they have drained, `settle` throws `operation_cancelled` instead of
/// returning outcomes. If starting a job fails (for example the deck task
/// table is full, or a job already has a completion observer), the started
/// jobs are stopped and drained, and that error is thrown. Throws
/// `runtime_error` for an empty task, before starting anything.
///
/// The predicate runs at each job's final suspension, so a stop it issues
/// reaches companions before their next step. See @ref rt_group.
///
/// @code
/// auto [a, b] = co_await nxtrt::settle(
///     std::tuple{fetch_a(), fetch_b()}, nxtrt::fail_fast_group{});
/// if (!a)
///     nxtrt::rethrow(a.error());
/// @endcode
template<typename... Ts, detail::group_policy Policy = all_group>
[[nodiscard]] task<std::tuple<outcome<Ts>...>>
settle(std::tuple<task<Ts>...> tasks, Policy execution = {})
{
    co_await detail::run_group(tasks, std::move(execution));
    co_return std::apply(
        [](auto &... child) {
            return std::tuple{
                detail::extract_outcome(child)...};
        },
        tasks);
}

/// Run a range of tasks concurrently and wait until every started one has
/// finished. Returns the outcomes in range order.
///
/// The range is stored by value in the returned task and drained into a
/// vector when that task first runs, before any job starts; a lazy view that
/// creates tasks works if whatever it refers to is still alive then. The
/// policy's `index` is the position in the range. Otherwise the
/// contract is that of `settle`. An empty range returns an empty vector.
template<
    std::ranges::input_range Range,
    detail::group_policy Policy = all_group>
    requires is_task_v<std::ranges::range_value_t<Range>>
[[nodiscard]] auto settle_range(Range range, Policy execution = {}) -> task<
    std::vector<outcome<task_result_t<std::ranges::range_value_t<Range>>>>>
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    auto tasks = std::vector<task<result_type>>{};
    for (auto && item : range)
        tasks.push_back(std::move(item));
    co_await detail::run_group(tasks, std::move(execution));

    auto out = std::vector<outcome<result_type>>{};
    out.reserve(tasks.size());
    for (auto & child : tasks)
        out.push_back(detail::extract_outcome(child));
    co_return out;
}

/// Run tasks concurrently and return all their values, in tuple order, with
/// `std::monostate` in place of `void` results.
///
/// Built on `settle` with `fail_fast_group`: the first job to fail stops
/// the others, and after every started job has drained, that first
/// failure (in completion order, not tuple order) is rethrown. Errors from
/// jobs that failed later, including the `operation_cancelled` of jobs it
/// stopped, are dropped; use `settle` to see them. Stopping the awaiting
/// task throws `operation_cancelled`. Also callable as
/// `when_all(a(), b(), ...)`.
///
/// @code
/// auto [n, text] = co_await nxtrt::when_all(count(), describe());
/// @endcode
template<typename... Ts>
[[nodiscard]] auto when_all(std::tuple<task<Ts>...> tasks)
    -> task<std::tuple<
        std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>>
{
    auto first = std::optional<std::size_t>{};
    auto outcomes = co_await settle(
        std::move(tasks), detail::record_first<fail_fast_group>{first});
    if (first)
        detail::rethrow_first_failure(
            outcomes, *first, std::index_sequence_for<Ts...>{});
    co_return detail::take_all_or_throw(
        outcomes, std::index_sequence_for<Ts...>{});
}

template<typename... Ts>
    requires(sizeof...(Ts) > 0)
[[nodiscard]] auto when_all(task<Ts>... tasks)
{
    return when_all(std::tuple{std::move(tasks)...});
}

/// Run same-typed tasks concurrently and return one successful result.
///
/// Built on `settle` with `first_success_group`: the first job to succeed
/// stops the others, and all started jobs drain before this returns. The
/// value returned belongs to the job whose success triggered the stop, even
/// if another job ignores cancellation and later succeeds. Tasks already
/// completed at entry are observed in tuple order. If extracting the
/// winner's result fails, that error is thrown; another success does not
/// replace it. Failures are skipped until a job succeeds. If every job
/// fails, their exceptions are thrown together through `throw_exceptions`
/// (an `exception_group`, or the single exception if there was one job).
/// Stopping the awaiting task throws `operation_cancelled`. Also callable
/// as `wait_any(a(), b(), ...)`.
template<typename T, typename... Ts>
    requires(std::same_as<T, Ts> && ...)
[[nodiscard]] task<T> wait_any(std::tuple<task<T>, task<Ts>...> tasks)
{
    auto first = std::optional<std::size_t>{};
    auto outcomes = co_await settle(
        std::move(tasks), detail::record_first<first_success_group>{first});
    auto positions = std::apply(
        [](auto &... value) { return std::array{&value...}; }, outcomes);
    if (first) {
        if constexpr (std::is_void_v<T>) {
            (void) detail::take_outcome(std::move(*positions[*first]));
            co_return;
        } else {
            co_return detail::take_outcome(std::move(*positions[*first]));
        }
    }
    auto exceptions = std::vector<std::exception_ptr>{};
    for (auto * value : positions)
        exceptions.push_back(value->error());
    throw_exceptions("wait_any tasks failed", std::move(exceptions));
    throw logic_error{"wait_any returned without result"};
}

template<typename T, typename... Ts>
    requires(std::same_as<T, Ts> && ...)
[[nodiscard]] task<T> wait_any(task<T> first, task<Ts>... rest)
{
    return wait_any(std::tuple{std::move(first), std::move(rest)...});
}

/// Range form of `when_all`: returns all values in range order. The first
/// failure to complete stops the rest and, after draining, is rethrown
/// (see `when_all`). Element tasks
/// must not be `task<void>`; use `settle_range` for those.
template<std::ranges::input_range Range>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && (!std::is_void_v<
            task_result_t<std::ranges::range_value_t<Range>>>)
[[nodiscard]] task<
    std::vector<task_result_t<std::ranges::range_value_t<Range>>>>
when_all_range(Range tasks)
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    auto first = std::optional<std::size_t>{};
    auto outcomes = co_await settle_range(
        std::move(tasks), detail::record_first<fail_fast_group>{first});
    if (first)
        rethrow(outcomes[*first].error());
    auto out = std::vector<result_type>{};
    out.reserve(outcomes.size());
    for (auto & outcome : outcomes)
        out.push_back(detail::take_outcome(std::move(outcome)));
    co_return out;
}

/// Await the tasks of a range one at a time, in order, discarding their
/// values. Not concurrent: each task is created (if the range is a lazy
/// view) and awaited only after the previous one finished. The first
/// failure propagates and the remaining tasks are not run.
template<std::ranges::input_range Range>
[[nodiscard]] task<void> for_each_task(Range tasks)
{
    for (auto child : tasks) {
        using child_type = std::remove_cvref_t<decltype(child)>;
        if constexpr (std::is_void_v<task_result_t<child_type>>) {
            co_await std::move(child);
        } else {
            (void)co_await std::move(child);
        }
    }
}

/// Range form of `wait_any`: returns the first success in completion order
/// once all started jobs have drained. If all fail, their exceptions are
/// thrown through `throw_exceptions`. Throws `runtime_error` for an empty
/// range.
template<std::ranges::input_range Range>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && (!std::is_void_v<
            task_result_t<std::ranges::range_value_t<Range>>>)
[[nodiscard]] task<task_result_t<std::ranges::range_value_t<Range>>>
wait_any_range(Range tasks)
{
    auto first = std::optional<std::size_t>{};
    auto outcomes = co_await settle_range(
        std::move(tasks), detail::record_first<first_success_group>{first});
    if (outcomes.empty())
        throw runtime_error{"wait_any_range used with no tasks"};

    if (first)
        co_return detail::take_outcome(std::move(outcomes[*first]));

    auto exceptions = std::vector<std::exception_ptr>{};
    for (auto & outcome : outcomes)
        exceptions.push_back(outcome.error());

    throw_exceptions("wait_any tasks failed", std::move(exceptions));
    throw logic_error{"wait_any_range returned without result"};
}

/// A task that waits `duration` on the deck's wand and then throws
/// `timeout_error`. It never completes normally; stopping it cancels the
/// timer wish. Needs a deck with a wand.
[[nodiscard]] inline task<void> timeout_after(
    std::chrono::nanoseconds duration)
{
    co_await op::timeout::after(duration);
    throw timeout_error{};
}

/// Run `body` with a deadline: return its result, or throw `timeout_error`
/// if `duration` passes first.
///
/// `body` and a `timeout_after(duration)` timer run in a `settle` group
/// with `first_completion_group`, so whichever settles first stops the
/// other, and both drain before this returns. The result is decided as:
/// - `body` succeeded: its value, even if the timer fired meanwhile;
/// - `body` failed with anything other than `operation_cancelled`: that
///   exception (an ordinary failure is never replaced by a timeout);
/// - `body` was cancelled because the timer fired: `timeout_error`;
/// - `body` ended with `operation_cancelled` on its own before the
///   deadline: `operation_cancelled`.
///
/// Stopping the task that awaits `with_timeout` stops both and throws
/// `operation_cancelled`, not `timeout_error`. Needs a deck with a wand for
/// the timer.
template<typename T>
[[nodiscard]] task<T> with_timeout(
    std::chrono::nanoseconds duration,
    task<T> body)
{
    auto outcomes = co_await settle(
        std::tuple{
            std::move(body),
            timeout_after(duration)},
        first_completion_group{});
    auto & body_result = std::get<0>(outcomes);
    if (body_result) {
        if constexpr (std::is_void_v<T>) {
            co_return;
        } else {
            co_return std::move(*body_result);
        }
    }

    auto & timeout_result = std::get<1>(outcomes);
    // An ordinary body failure cancels the timer; do not replace that
    // failure with the timer's cancellation. A real deadline wins over
    // cancellation of the body, while external stop remains cancellation.
    if (!is_operation_cancelled(body_result.error()))
        rethrow(body_result.error());
    if (!timeout_result)
        rethrow(timeout_result.error());
    rethrow(body_result.error());

    if constexpr (std::is_void_v<T>) {
        co_return;
    } else {
        throw logic_error{"nxtrt with_timeout returned without result"};
    }
}

} // namespace nxtrt
