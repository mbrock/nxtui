#pragma once

// Groups: run a fixed set or a range of tasks concurrently. Subclasses decide
// when the rest are cancelled; every started job settles before return.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/compose.hpp"

#include <expected>
#include <span>
#include <vector>

namespace nxtrt {

/// How one job of a group settled: its value, or the exception it ended with.
template<typename T>
using outcome = std::expected<T, std::exception_ptr>;

namespace detail {
struct group_child;
}

/// Shared execution state for a group. Subclasses decide when to stop the
/// remaining jobs; every started job is drained before run() returns.
/// A group may be moved before execution, but not while bound to children.
class group
{
public:
    group() = default;
    group(const group &) = delete;
    group & operator=(const group &) = delete;
    group(group && other) noexcept;
    virtual ~group();

    /// Execution entry used by settle; records must be fresh and remain alive
    /// until this task finishes. Observers are detached before it returns.
    task<void> run(std::span<detail::group_child> children);

protected:
    virtual bool should_stop(std::size_t index, bool failed) const noexcept = 0;

private:
    friend struct detail::group_child;
    std::span<detail::group_child> children_;
    deck * executor_ = nullptr;
    std::size_t pending_ = 0;
    need waiter_{};
    bool stopped_ = false;
    bool parent_stopped_ = false;

    void stop() noexcept;
    void signal() noexcept;
    void completed(detail::group_child & child) noexcept;
    void detach() noexcept;

    struct stop_callback;
    struct drain_awaiter;
};

/// Let every job run to completion.
struct all_group : group
{
    bool should_stop(std::size_t, bool) const noexcept override
    {
        return false;
    }
};

/// Stop the others when a job fails.
struct fail_fast_group : group
{
    bool should_stop(std::size_t, bool failed) const noexcept override
    {
        return failed;
    }
};

/// Stop the others when a job succeeds.
struct first_success_group : group
{
    bool should_stop(std::size_t, bool failed) const noexcept override
    {
        return !failed;
    }
};

/// Stop the others when any job settles.
struct first_completion_group : group
{
    bool should_stop(std::size_t, bool) const noexcept override
    {
        return true;
    }
};

/// Stop the companions when the first job (the primary) settles.
struct primary_group : group
{
    bool should_stop(std::size_t index, bool) const noexcept override
    {
        return index == 0;
    }
};

namespace detail {

template<typename T>
[[nodiscard]] outcome<T> cancelled_outcome()
{
    return outcome<T>{
        std::unexpected{std::make_exception_ptr(operation_cancelled{})}};
}

/// Stable observers borrow tasks owned by the settle frame. Values stay in
/// their promises until every started task has reached final suspension.
struct group_child final : completion_observer
{
    std::coroutine_handle<> handle;
    promise_base * promise = nullptr;
    bool (*failed)(promise_base *) noexcept = nullptr;
    group * owner = nullptr;
    std::size_t index = 0;
    bool started = false;
    bool completed = false;

    template<typename T>
    void bind(task<T> & child) noexcept
    {
        handle = child.handle();
        if (handle)
            promise = &child.handle().promise();
        failed = [](promise_base * base) noexcept {
            try {
                static_cast<detail::promise<T> *>(base)->result();
                return false;
            } catch (...) {
                return true;
            }
        };
    }

    void task_completed() noexcept override;
};

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
template<typename... Tasks, typename Group = all_group>
    requires (is_task_v<Tasks> && ...) && std::derived_from<Group, group>
[[nodiscard]] task<std::tuple<outcome<task_result_t<Tasks>>...>>
settle(std::tuple<Tasks...> tasks, Group execution = {})
{
    auto children = std::array<detail::group_child, sizeof...(Tasks)>{};
    auto index = std::size_t{0};
    std::apply([&](auto &... child) {
        (children[index++].bind(child), ...);
    }, tasks);
    co_await execution.run(std::span{children});
    co_return std::apply(
        [](auto &... child) {
            return std::tuple{
                detail::extract_outcome(child)...};
        },
        tasks);
}

/// Run a range of tasks concurrently and settle all of it.
/// Returns the outcomes in range order.
template<std::ranges::input_range Range, typename Group = all_group>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && std::derived_from<Group, group>
[[nodiscard]] auto settle_range(Range range, Group execution = {})
    -> task<std::vector<outcome<
        task_result_t<std::ranges::range_value_t<Range>>>>>
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    auto tasks = std::vector<task<result_type>>{};
    for (auto && item : range)
        tasks.push_back(std::move(item));
    auto children = std::vector<detail::group_child>(tasks.size());
    for (auto i = std::size_t{0}; i < tasks.size(); ++i)
        children[i].bind(tasks[i]);
    co_await execution.run(std::span{children});

    auto out = std::vector<outcome<result_type>>{};
    out.reserve(tasks.size());
    for (auto & child : tasks)
        out.push_back(detail::extract_outcome(child));
    co_return out;
}

/// All results in tuple order; void positions are monostate. The first
/// failure stops the rest and is rethrown.
template<typename... Tasks>
    requires(is_task_v<Tasks> && ...)
[[nodiscard]] auto when_all(std::tuple<Tasks...> tasks)
    -> task<std::tuple<std::conditional_t<
        std::is_void_v<task_result_t<Tasks>>,
        std::monostate,
        task_result_t<Tasks>>...>>
{
    auto outcomes = co_await settle(std::move(tasks), fail_fast_group{});
    co_return detail::take_all_or_throw(
        outcomes, std::index_sequence_for<Tasks...>{});
}

template<typename... Tasks>
    requires(sizeof...(Tasks) > 0) && (is_task_v<Tasks> && ...)
[[nodiscard]] auto when_all(Tasks... tasks)
{
    return when_all(std::tuple{std::move(tasks)...});
}

/// First success in tuple order, not first completion; the first success
/// stops the rest. All failures are grouped.
template<typename First, typename... Rest>
    requires is_task_v<First>
            && (is_task_v<Rest> && ...)
            && (std::same_as<
                    task_result_t<First>,
                    task_result_t<Rest>>
                && ...)
[[nodiscard]] task<task_result_t<First>>
wait_any(std::tuple<First, Rest...> tasks)
{
    using result_type = task_result_t<First>;
    auto outcomes = co_await settle(std::move(tasks), first_success_group{});
    if constexpr (std::is_void_v<result_type>) {
        detail::take_first_void_success_or_throw(
            outcomes, std::index_sequence_for<First, Rest...>{});
    } else {
        co_return detail::take_first_success_or_throw<result_type>(
            outcomes, std::index_sequence_for<First, Rest...>{});
    }
}

template<typename T, typename... Rest>
    requires (std::same_as<task<T>, std::remove_cvref_t<Rest>> && ...)
[[nodiscard]] task<T> wait_any(task<T> first, Rest... rest)
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
    auto body_result = std::move(std::get<0>(outcomes));
    if (body_result) {
        if constexpr (std::is_void_v<T>) {
            co_return;
        } else {
            co_return std::move(*body_result);
        }
    }

    auto timeout_result = std::move(std::get<1>(outcomes));
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
