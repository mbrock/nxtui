#pragma once

// Groups: run a fixed set or a range of ideas concurrently in a pool, with a
// stop rule deciding when the rest are cancelled. A group settles every job
// before it returns. Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/compose.hpp"
#include "nxtrt/pool.hpp"

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

template<typename Work>
    requires(is_task_v<Work> || stored_task_factory<Work>)
[[nodiscard]] auto start_work(Work & work)
{
    if constexpr (is_task_v<Work>)
        return std::move(work);
    else
        return std::invoke(work);
}

template<typename Work>
using work_result_t =
    task_result_t<decltype(start_work(std::declval<Work &>()))>;

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
    void * jobs = nullptr;
    void (*stop_jobs)(void *) noexcept = nullptr;

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
        if (stop_jobs != nullptr)
            stop_jobs(jobs);
    }
};

/// One job of a group: an index into the group and the function that starts
/// it. Every group uses this one recipe type, so one pool type serves all.
struct group_recipe
{
    void * group = nullptr;
    std::size_t index = 0;
    task<void> (*invoke)(void * group, std::size_t index) = nullptr;

    task<void> operator()() &
    {
        return invoke(group, index);
    }
};

template<typename T>
task<void> settle_job(
    group_control & control,
    std::size_t index,
    std::optional<outcome<T>> & result,
    task<T> child)
{
    try {
        if constexpr (std::is_void_v<T>) {
            co_await child;
            result.emplace(std::in_place);
        } else {
            result.emplace(std::in_place, co_await child);
        }
    } catch (...) {
        result.emplace(std::unexpected{std::current_exception()});
    }
    control.settled(index, !*result);
}

template<typename T>
task<void> start_job(
    group_control & control,
    std::size_t index,
    std::optional<outcome<T>> & result,
    task<T> child)
{
    if (!child.handle())
        throw runtime_error{"nxtrt group recipe returned an empty task"};
    return settle_job(control, index, result, std::move(child));
}

/// Run a group's recipes in a pool with one slot per recipe. A stop chosen
/// by the rule is a normal finish; outside cancellation is not. Defined in
/// nxtrt/group.cpp, so it is compiled once rather than in every user.
task<void> run_group(std::span<group_recipe> recipes, group_control & control);

// Heterogeneity lives in the result tuple, not the pool: every indexed
// recipe produces void work for the same ordinary pool.
template<typename... Work>
struct tuple_group
{
    group_control & control;
    std::tuple<Work...> & work;
    std::tuple<std::optional<outcome<work_result_t<Work>>>...> results;

    template<std::size_t I>
    static task<void> start(void * group, std::size_t)
    {
        auto & self = *static_cast<tuple_group *>(group);
        return start_job(
            self.control,
            I,
            std::get<I>(self.results),
            start_work(std::get<I>(self.work)));
    }

    template<std::size_t... Is>
    auto recipes(std::index_sequence<Is...>)
    {
        return std::array<group_recipe, sizeof...(Is)>{
            group_recipe{this, Is, start<Is>}...};
    }
};

template<typename T, typename Work>
struct range_group
{
    group_control & control;
    std::vector<Work> work;
    std::vector<std::optional<outcome<T>>> results;

    static task<void> start(void * group, std::size_t index)
    {
        auto & self = *static_cast<range_group *>(group);
        return start_job(
            self.control,
            index,
            self.results[index],
            start_work(self.work[index]));
    }
};

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

/// Run a fixed set of work concurrently and settle all of it. Elements are
/// tasks or task factories (ideas); a factory is invoked once, when its job
/// starts, and the tuple keeps it alive until the group returns. `rule`
/// decides when the remaining jobs are stopped. Returns each job's outcome
/// in tuple order; a job stopped before it started settles as cancelled.
template<typename... Work, stop_rule Rule = settle_all>
    requires((is_task_v<Work> || stored_task_factory<Work>) && ...)
[[nodiscard]] task<std::tuple<outcome<detail::work_result_t<Work>>...>>
settle(std::tuple<Work...> work, Rule rule = {})
{
    auto control = detail::group_control{rule};
    auto group = detail::tuple_group<Work...>{control, work, {}};
    auto recipes = group.recipes(std::index_sequence_for<Work...>{});
    co_await detail::run_group(
        std::span<detail::group_recipe>{recipes}, control);
    co_return std::apply(
        []<typename... T>(std::optional<T> &... result) {
            return std::tuple{
                (result ? std::move(*result)
                        : T{std::unexpected{std::make_exception_ptr(
                              operation_cancelled{})}})...};
        },
        group.results);
}

/// Run a range of work (tasks or ideas) concurrently and settle all of it.
/// Returns the outcomes in range order.
template<std::ranges::input_range Range, stop_rule Rule = settle_all>
    requires(is_task_v<std::ranges::range_value_t<Range>>
             || stored_task_factory<std::ranges::range_value_t<Range>>)
[[nodiscard]] auto settle_range(Range range, Rule rule = {})
    -> task<std::vector<outcome<
        detail::work_result_t<std::ranges::range_value_t<Range>>>>>
{
    using work_type = std::ranges::range_value_t<Range>;
    using result_type = detail::work_result_t<work_type>;
    using group_type = detail::range_group<result_type, work_type>;

    auto control = detail::group_control{rule};
    auto group = group_type{control, {}, {}};
    for (auto && item : range)
        group.work.push_back(std::move(item));
    group.results.resize(group.work.size());
    auto recipes = std::vector<detail::group_recipe>{};
    recipes.reserve(group.work.size());
    for (auto i = std::size_t{0}; i < group.work.size(); ++i)
        recipes.push_back({&group, i, group_type::start});

    co_await detail::run_group(std::span{recipes}, control);

    auto out = std::vector<outcome<result_type>>{};
    out.reserve(group.results.size());
    for (auto & result : group.results)
        out.push_back(
            result ? std::move(*result)
                   : detail::cancelled_outcome<result_type>());
    co_return out;
}

/// All results in tuple order; void positions are monostate. The first
/// failure stops the rest and is rethrown.
template<typename... Work>
    requires((is_task_v<Work> || stored_task_factory<Work>) && ...)
[[nodiscard]] auto when_all(std::tuple<Work...> work)
    -> task<std::tuple<std::conditional_t<
        std::is_void_v<detail::work_result_t<Work>>,
        std::monostate,
        detail::work_result_t<Work>>...>>
{
    auto outcomes = co_await settle(std::move(work), stop_on_failure{});
    co_return detail::take_all_or_throw(
        outcomes, std::index_sequence_for<Work...>{});
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
    requires(is_task_v<First> || stored_task_factory<First>)
            && ((is_task_v<Rest> || stored_task_factory<Rest>) && ...)
            && (std::same_as<
                    detail::work_result_t<First>,
                    detail::work_result_t<Rest>>
                && ...)
[[nodiscard]] task<detail::work_result_t<First>>
wait_any(std::tuple<First, Rest...> work)
{
    using result_type = detail::work_result_t<First>;
    auto outcomes = co_await settle(std::move(work), stop_on_success{});
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
            [duration] { return timeout_after(duration); }},
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
