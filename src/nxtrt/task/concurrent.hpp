#pragma once

// Groups: run a fixed set or a range of tasks concurrently. Predicates
// decide when the rest are cancelled; every started job settles before
// return. Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/compose.hpp"

#include <expected>
#include <vector>

namespace nxtrt {

/// How one job of a group settled: its value, or the exception it ended with.
template<typename T>
using outcome = std::expected<T, std::exception_ptr>;

/// Let every job run to completion.
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

/// Stop the companions when the first job (the primary) settles.
struct primary_group
{
    bool operator()(std::size_t index, bool) const noexcept
    {
        return index == 0;
    }
};

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

template<typename T, typename Tuple, std::size_t... Is>
[[nodiscard]] T
take_first_success_or_throw(Tuple & outcomes, std::index_sequence<Is...>)
{
    auto exceptions = std::vector<std::exception_ptr>{};
    auto result = std::optional<T>{};

    auto inspect = [&](auto index) {
        if (result)
            return;

        auto value = std::move(std::get<index>(outcomes));
        if (value) {
            result.emplace(std::move(*value));
        } else {
            exceptions.push_back(value.error());
        }
    };

    (inspect(std::integral_constant<std::size_t, Is>{}), ...);

    if (result)
        return std::move(*result);
    throw_exceptions("wait_any tasks failed", std::move(exceptions));
}

template<typename Tuple, std::size_t... Is>
void take_first_void_success_or_throw(
    Tuple & outcomes, std::index_sequence<Is...>)
{
    auto exceptions = std::vector<std::exception_ptr>{};
    auto succeeded = false;

    auto inspect = [&](auto index) {
        if (succeeded)
            return;

        auto value = std::move(std::get<index>(outcomes));
        if (value) {
            succeeded = true;
        } else {
            exceptions.push_back(value.error());
        }
    };

    (inspect(std::integral_constant<std::size_t, Is>{}), ...);

    if (succeeded)
        return;
    throw_exceptions("wait_any tasks failed", std::move(exceptions));
}

} // namespace detail

/// Run a fixed set of tasks concurrently and settle all of it. `execution`
/// decides when the remaining jobs are stopped. Returns each job's outcome
/// in tuple order; a job stopped before it started settles as cancelled.
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

/// Run a range of tasks concurrently and settle all of it.
/// Returns the outcomes in range order.
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

/// All results in tuple order; void positions are monostate. The first
/// failure stops the rest and is rethrown.
template<typename... Ts>
[[nodiscard]] auto when_all(std::tuple<task<Ts>...> tasks)
    -> task<std::tuple<
        std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>>
{
    auto outcomes = co_await settle(std::move(tasks), fail_fast_group{});
    co_return detail::take_all_or_throw(
        outcomes, std::index_sequence_for<Ts...>{});
}

template<typename... Ts>
    requires(sizeof...(Ts) > 0)
[[nodiscard]] auto when_all(task<Ts>... tasks)
{
    return when_all(std::tuple{std::move(tasks)...});
}

/// First success in tuple order, not first completion; the first success
/// stops the rest. All failures are grouped.
template<typename T, typename... Ts>
    requires(std::same_as<T, Ts> && ...)
[[nodiscard]] task<T> wait_any(std::tuple<task<T>, task<Ts>...> tasks)
{
    auto outcomes = co_await settle(std::move(tasks), first_success_group{});
    if constexpr (std::is_void_v<T>) {
        detail::take_first_void_success_or_throw(
            outcomes, std::make_index_sequence<1 + sizeof...(Ts)>{});
    } else {
        co_return detail::take_first_success_or_throw<T>(
            outcomes, std::make_index_sequence<1 + sizeof...(Ts)>{});
    }
}

template<typename T, typename... Ts>
    requires(std::same_as<T, Ts> && ...)
[[nodiscard]] task<T> wait_any(task<T> first, task<Ts>... rest)
{
    return wait_any(std::tuple{std::move(first), std::move(rest)...});
}

/// All results in range order. The first failure stops the rest and is
/// rethrown.
template<std::ranges::input_range Range>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && (!std::is_void_v<
            task_result_t<std::ranges::range_value_t<Range>>>)
[[nodiscard]] task<
    std::vector<task_result_t<std::ranges::range_value_t<Range>>>>
when_all_range(Range tasks)
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    auto outcomes = co_await settle_range(std::move(tasks), fail_fast_group{});
    auto out = std::vector<result_type>{};
    out.reserve(outcomes.size());
    for (auto & outcome : outcomes)
        out.push_back(detail::take_outcome(std::move(outcome)));
    co_return out;
}

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

/// First success in range order; the first success stops the rest. All
/// failures are grouped.
template<std::ranges::input_range Range>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && (!std::is_void_v<
            task_result_t<std::ranges::range_value_t<Range>>>)
[[nodiscard]] task<task_result_t<std::ranges::range_value_t<Range>>>
wait_any_range(Range tasks)
{
    auto outcomes = co_await settle_range(
        std::move(tasks), first_success_group{});
    if (outcomes.empty())
        throw runtime_error{"wait_any_range used with no tasks"};

    auto exceptions = std::vector<std::exception_ptr>{};
    for (auto & outcome : outcomes) {
        if (outcome)
            co_return std::move(*outcome);
        exceptions.push_back(outcome.error());
    }

    throw_exceptions("wait_any tasks failed", std::move(exceptions));
    throw logic_error{"wait_any_range returned without result"};
}

[[nodiscard]] inline task<void> timeout_after(
    std::chrono::nanoseconds duration)
{
    co_await op::timeout::after(duration);
    throw timeout_error{};
}

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
