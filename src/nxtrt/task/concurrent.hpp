#pragma once

// Groups: run a fixed set or a range of tasks concurrently, with a
// stop rule deciding when the rest are cancelled. A group settles every job
// before it returns. Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/compose.hpp"

#include <expected>
#include <span>
#include <vector>

namespace nxtrt {

/// How one job of a group settled: its value, or the exception it ended with.
template<typename T>
using outcome = std::expected<T, std::exception_ptr>;

/// Stop rules decide, as each job of a group settles, whether to stop the
/// others. `index` is the job's position in the group.
template<typename Rule>
concept stop_rule =
    std::is_nothrow_invocable_r_v<bool, const Rule &, std::size_t, bool>;

/// Let every job run to completion.
struct settle_all
{
    bool operator()(std::size_t, bool) const noexcept
    {
        return false;
    }
};

/// Stop the others when a job fails.
struct stop_on_failure
{
    bool operator()(std::size_t, bool failed) const noexcept
    {
        return failed;
    }
};

/// Stop the others when a job succeeds.
struct stop_on_success
{
    bool operator()(std::size_t, bool failed) const noexcept
    {
        return !failed;
    }
};

/// Stop the others when any job settles.
struct stop_on_completion
{
    bool operator()(std::size_t, bool) const noexcept
    {
        return true;
    }
};

/// Stop the companions when the first job (the primary) settles.
struct stop_after_first
{
    bool operator()(std::size_t index, bool) const noexcept
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

/// Applies a group's stop rule as its jobs settle. The rule is reached
/// through a function pointer so the group machinery below is shared by
/// every group rather than instantiated per rule and per call site.
struct group_control
{
    const void * rule = nullptr;
    bool (*should_stop)(const void *, std::size_t, bool) noexcept = nullptr;
    bool stopped = false;

    template<typename Rule>
    explicit group_control(const Rule & rule) noexcept
        : rule(&rule)
        , should_stop([](const void * rule,
                         std::size_t index,
                         bool failed) noexcept {
            return (*static_cast<const Rule *>(rule))(index, failed);
        })
    {}

    void settled(std::size_t index, bool failed) noexcept
    {
        if (stopped || !should_stop(rule, index, failed))
            return;
        stopped = true;
    }
};

struct group_coordinator;

/// Stable observers borrow tasks owned by the settle frame. Values stay in
/// their promises until every started task has reached final suspension.
struct group_child final : completion_observer
{
    std::coroutine_handle<> handle;
    promise_base * promise = nullptr;
    bool (*failed)(promise_base *) noexcept = nullptr;
    group_coordinator * owner = nullptr;
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

task<void> run_group(std::span<group_child> children, group_control & control);

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

/// Run a fixed set of tasks concurrently and settle all of it. `rule`
/// decides when the remaining jobs are stopped. Returns each job's outcome
/// in tuple order; a job stopped before it started settles as cancelled.
template<typename... Tasks, stop_rule Rule = settle_all>
    requires(is_task_v<Tasks> && ...)
[[nodiscard]] task<std::tuple<outcome<task_result_t<Tasks>>...>>
settle(std::tuple<Tasks...> tasks, Rule rule = {})
{
    auto control = detail::group_control{rule};
    auto children = std::array<detail::group_child, sizeof...(Tasks)>{};
    auto index = std::size_t{0};
    std::apply([&](auto &... child) {
        (children[index++].bind(child), ...);
    }, tasks);
    co_await detail::run_group(std::span{children}, control);
    co_return std::apply(
        [](auto &... child) {
            return std::tuple{
                detail::extract_outcome(child)...};
        },
        tasks);
}

/// Run a range of tasks concurrently and settle all of it.
/// Returns the outcomes in range order.
template<std::ranges::input_range Range, stop_rule Rule = settle_all>
    requires is_task_v<std::ranges::range_value_t<Range>>
[[nodiscard]] auto settle_range(Range range, Rule rule = {})
    -> task<std::vector<outcome<
        task_result_t<std::ranges::range_value_t<Range>>>>>
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    auto control = detail::group_control{rule};
    auto tasks = std::vector<task<result_type>>{};
    for (auto && item : range)
        tasks.push_back(std::move(item));
    auto children = std::vector<detail::group_child>(tasks.size());
    for (auto i = std::size_t{0}; i < tasks.size(); ++i)
        children[i].bind(tasks[i]);
    co_await detail::run_group(std::span{children}, control);

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
    auto outcomes = co_await settle(std::move(tasks), stop_on_failure{});
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
    auto outcomes = co_await settle(std::move(tasks), stop_on_success{});
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
    auto outcomes = co_await settle_range(std::move(tasks), stop_on_failure{});
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
    auto outcomes = co_await settle_range(std::move(tasks), stop_on_success{});
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
        stop_on_completion{});
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
