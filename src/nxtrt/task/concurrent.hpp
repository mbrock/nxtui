#pragma once

// Sibling cancellation policies, concurrent composition, and timeouts.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/compose.hpp"
#include "nxtrt/task/scope.hpp"
#include "nxtrt/pool.hpp"

namespace nxtrt {

class stop_on_failure : public firm
{
public:
    using firm::firm;
    stop_on_failure() = default;
    stop_on_failure(stop_on_failure &&) noexcept = default;
    stop_on_failure & operator=(stop_on_failure &&) = delete;

    [[nodiscard]] std::exception_ptr first_failure() const noexcept
    {
        return first_failure_;
    }

    void completed(task_id, std::exception_ptr failure) noexcept override
    {
        if (!failure)
            return;
        if (!first_failure_)
            first_failure_ = failure;
        stop();
    }

private:
    std::exception_ptr first_failure_;
};

class stop_on_success : public firm
{
public:
    using firm::firm;
    stop_on_success() = default;
    stop_on_success(stop_on_success &&) noexcept = default;
    stop_on_success & operator=(stop_on_success &&) = delete;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return succeeded_;
    }

    void completed(task_id, std::exception_ptr failure) noexcept override
    {
        if (failure)
            return;
        succeeded_ = true;
        stop();
    }

private:
    bool succeeded_ = false;
};

/// Stop siblings on either success or failure; always join before
/// returning.
class stop_on_completion : public firm
{
public:
    using firm::firm;

    void completed(task_id, std::exception_ptr) noexcept override
    {
        stop();
    }
};

namespace detail {

template<typename Work>
    requires(is_task_v<Work> || stored_task_factory<Work>)
[[nodiscard]] auto start_firm_work(Work & work)
{
    if constexpr (is_task_v<Work>)
        return std::move(work);
    else
        return std::invoke(work);
}

template<typename Work>
using firm_work_result_t =
    task_result_t<decltype(start_firm_work(std::declval<Work &>()))>;

template<typename T>
using outcome = std::expected<T, std::exception_ptr>;

// Heterogeneity lives in the result tuple, not the execution owner. Every
// indexed recipe produces void work for the same ordinary pool.
template<typename Policy, typename... Work>
struct tuple_work
{
    Policy & policy;
    std::tuple<Work...> & work;
    std::tuple<std::optional<outcome<firm_work_result_t<Work>>>...> results;

    template<std::size_t I, typename T>
    static task<void> run(tuple_work & batch, task<T> child)
    {
        auto & result = std::get<I>(batch.results);
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
        batch.policy.completed(
            child.id(), *result ? std::exception_ptr{} : result->error());
    }

    template<std::size_t I>
    static task<void> start(tuple_work & batch)
    {
        auto child = start_firm_work(std::get<I>(batch.work));
        if (!child.handle())
            throw runtime_error{
                "nxtrt tuple recipe returned an empty task"};
        return run<I>(batch, std::move(child));
    }

    struct recipe
    {
        tuple_work * batch;
        task<void> (*invoke)(tuple_work &);

        task<void> operator()() &
        {
            return invoke(*batch);
        }
    };

    template<std::size_t... Is>
    auto recipes(std::index_sequence<Is...>)
    {
        return std::array<recipe, sizeof...(Is)>{
            recipe{this, start<Is>}...};
    }
};

template<typename Recipe>
task<void> consume_work(pool<Recipe> & work)
{
    while (co_await work.take())
        ;
}

template<typename Policy, typename... Work>
task<std::tuple<outcome<firm_work_result_t<Work>>...>>
run_firm_work(Policy & policy, std::tuple<Work...> & work)
{
    if (policy.stop_requested())
        throw operation_cancelled{};
    auto batch = tuple_work<Policy, Work...>{policy, work, {}};
    if constexpr (sizeof...(Work) != 0) {
        auto recipes = batch.recipes(std::index_sequence_for<Work...>{});
        using recipe = typename decltype(batch)::recipe;
        auto input_land = static_value_storage<recipe, 1>{};
        auto input = value_range_source{recipes, input_land.ref()};
        auto slots = std::array<pool_slot<recipe>, sizeof...(Work)>{};
        auto available = farm<pool_slot<recipe>, sizeof...(Work)>{&slots};
        auto output =
            static_value_storage<std::monostate, sizeof...(Work)>{};
        auto pending = pool<recipe>{input, available, output.ref()};
        auto stop_pool = [&pending] { pending.stop(); };
        auto on_stop = std::stop_callback{policy.stop_token(), stop_pool};
        try {
            co_await finally(consume_work(pending), [&pending] {
                return pending.close();
            });
        } catch (const operation_cancelled &) {
            // A policy-selected finish is normal; external cancellation is
            // not. In both cases finally has already drained the pool.
            if (!policy.stop_requested() || task_stop_requested())
                throw;
        }
    }
    // Explicitly owned nested forks remain outside the fixed batch.
    co_await policy.join();
    if (task_stop_requested())
        throw operation_cancelled{};
    co_return std::apply(
        []<typename... T>(std::optional<T> &... result) {
            // A factory may stop the scope before its task or later recipes
            // start. Those positions settle as cancellation, not missing
            // values.
            return std::tuple{
                (result ? std::move(*result)
                        : T{std::unexpected{std::make_exception_ptr(
                              operation_cancelled{})}})...};
        },
        batch.results);
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

// A primary activity may stop companions without making every companion
// completion a winner (unlike stop_on_completion).
template<typename T>
task<T> stop_firm_on_completion(task<T> child)
{
    try {
        if constexpr (std::is_void_v<T>) {
            co_await child;
            require_current_firm().stop();
            co_return;
        } else {
            auto value = co_await child;
            require_current_firm().stop();
            co_return value;
        }
    } catch (...) {
        if (auto * firm = current_firm())
            firm->stop();
        throw;
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

template<typename T>
[[nodiscard]] auto take_deed_result(catching_deed<T> deed)
{
    return take_outcome(std::move(deed).get());
}

template<typename Tuple, std::size_t... Is>
[[nodiscard]] auto
take_all_or_throw(Tuple & outcomes, std::index_sequence<Is...>)
{
    return std::tuple{
        take_outcome(std::move(std::get<Is>(outcomes)))...,
    };
}

} // namespace detail

/// Execute a fixed heterogeneous batch through a finite indexed pool.
/// Elements are tasks or owned nullary task factories. Factories run once,
/// under the new firm, and survive all child settlement (including
/// failure). Preconstructed tasks retain their original frame allocation;
/// both forms run under this firm's environment. Additional children
/// require an explicitly supplied owner, not an ambient spawn operation.
/// Main work is pool-owned, without child records/deeds. Returns a tuple of
/// settled expected outcomes; the policy controls sibling cancellation.
template<typename Policy = firm, typename... Work>
    requires std::derived_from<Policy, firm>
             && std::default_initializable<Policy>
             && ((is_task_v<Work> || stored_task_factory<Work>) && ...)
[[nodiscard]] task<
    std::tuple<detail::outcome<detail::firm_work_result_t<Work>>...>>
with_firm(std::tuple<Work...> work)
{
    co_return co_await with_firm<Policy>([&work](Policy & policy) {
        return detail::run_firm_work(policy, work);
    });
}

/// Tuple all: results retain tuple order; void positions are monostate.
template<typename... Work>
    requires((is_task_v<Work> || stored_task_factory<Work>) && ...)
[[nodiscard]] auto when_all(std::tuple<Work...> work)
    -> task<std::tuple<std::conditional_t<
        std::is_void_v<detail::firm_work_result_t<Work>>,
        std::monostate,
        detail::firm_work_result_t<Work>>...>>
{
    auto outcomes = co_await with_firm<stop_on_failure>(std::move(work));
    co_return detail::take_all_or_throw(
        outcomes, std::index_sequence_for<Work...>{});
}

/// Tuple first success, not first completion; all failures are grouped.
template<typename First, typename... Rest>
    requires(is_task_v<First> || stored_task_factory<First>)
            && ((is_task_v<Rest> || stored_task_factory<Rest>) && ...)
            && (std::same_as<
                    detail::firm_work_result_t<First>,
                    detail::firm_work_result_t<Rest>>
                && ...)
[[nodiscard]] task<detail::firm_work_result_t<First>>
wait_any(std::tuple<First, Rest...> work)
{
    using result_type = detail::firm_work_result_t<First>;
    auto outcomes = co_await with_firm<stop_on_success>(std::move(work));
    if constexpr (std::is_void_v<result_type>) {
        detail::take_first_void_success_or_throw(
            outcomes, std::index_sequence_for<First, Rest...>{});
    } else {
        co_return detail::take_first_success_or_throw<result_type>(
            outcomes, std::index_sequence_for<First, Rest...>{});
    }
}

template<typename... Tasks>
    requires(sizeof...(Tasks) > 0) && (is_task_v<Tasks> && ...)
[[nodiscard]] auto when_all(Tasks... tasks)
{
    return when_all(std::tuple{std::move(tasks)...});
}

template<std::ranges::input_range Range>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && (!std::is_void_v<
            task_result_t<std::ranges::range_value_t<Range>>>)
[[nodiscard]] task<
    std::vector<task_result_t<std::ranges::range_value_t<Range>>>>
when_all_range(Range tasks)
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    using deed_type = catching_deed<result_type>;

    auto deeds = co_await with_firm<stop_on_failure>(
        [tasks = std::move(tasks)](
            auto & policy) mutable -> task<std::vector<deed_type>> {
            auto out = std::vector<deed_type>{};
            for (auto child : tasks)
                out.push_back(policy.fork(std::move(child)).cope());
            co_await policy.join();
            co_return out;
        });

    auto out = std::vector<result_type>{};
    out.reserve(deeds.size());
    for (auto & deed : deeds)
        out.push_back(detail::take_deed_result(std::move(deed)));
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

template<std::ranges::input_range Range>
    requires is_task_v<std::ranges::range_value_t<Range>>
        && (!std::is_void_v<
            task_result_t<std::ranges::range_value_t<Range>>>)
[[nodiscard]] task<task_result_t<std::ranges::range_value_t<Range>>>
wait_any_range(Range tasks)
{
    using result_type = task_result_t<std::ranges::range_value_t<Range>>;
    using deed_type = catching_deed<result_type>;

    auto deeds = co_await with_firm<stop_on_success>(
        [tasks = std::move(tasks)](
            auto & policy) mutable -> task<std::vector<deed_type>> {
            auto out = std::vector<deed_type>{};
            for (auto child : tasks)
                out.push_back(policy.fork(std::move(child)).cope());
            if (out.empty())
                throw runtime_error{"wait_any_range used with no tasks"};
            co_await policy.join();
            co_return out;
        });

    auto exceptions = std::vector<std::exception_ptr>{};
    for (auto & deed : deeds) {
        auto value = std::move(deed).get();
        if (value)
            co_return std::move(*value);
        exceptions.push_back(value.error());
    }

    throw_exceptions("wait_any tasks failed", std::move(exceptions));
    throw logic_error{"wait_any_range returned without result"};
}

template<typename T, typename... Rest>
    requires (std::same_as<task<T>, std::remove_cvref_t<Rest>> && ...)
[[nodiscard]] task<T> wait_any(task<T> first, Rest... rest)
{
    return wait_any(std::tuple{std::move(first), std::move(rest)...});
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
    auto outcomes = co_await with_firm<stop_on_completion>(std::tuple{
        std::move(body), [duration] { return timeout_after(duration); }});
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
