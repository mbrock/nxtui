#pragma once

// Sibling cancellation policies, concurrent composition, and timeouts.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/compose.hpp"
#include "nxtrt/task/scope.hpp"

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

private:
    void child_finished(
        detail::child_record_base &,
        std::exception_ptr failure) noexcept override
    {
        if (!failure)
            return;
        if (!first_failure_)
            first_failure_ = failure;
        stop();
    }

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

private:
    void child_finished(
        detail::child_record_base &,
        std::exception_ptr failure) noexcept override
    {
        if (failure)
            return;
        succeeded_ = true;
        stop();
    }

    bool succeeded_ = false;
};

/// Stop siblings on either success or failure; always join before
/// returning.
class stop_on_completion : public firm
{
public:
    using firm::firm;

private:
    void child_finished(
        detail::child_record_base &, std::exception_ptr) noexcept override
    {
        stop();
    }
};

namespace detail {

template<typename Base, typename Fn>
class firm_body : public Base
{
public:
    explicit firm_body(Fn fn)
        : fn_(std::move(fn))
    {}

    auto operator()()
    {
        return std::invoke(fn_, static_cast<Base &>(*this));
    }

private:
    Fn fn_;
};

template<typename Base, typename Fn>
[[nodiscard]] firm_body<Base, std::decay_t<Fn>> make_firm_body(Fn && fn)
{
    return firm_body<Base, std::decay_t<Fn>>{std::forward<Fn>(fn)};
}

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

template<typename Policy, typename... Work>
task<std::tuple<catching_deed<firm_work_result_t<Work>>...>>
run_firm_work(Policy & policy, std::tuple<Work...> & work)
{
    using deeds_type =
        std::tuple<catching_deed<firm_work_result_t<Work>>...>;
    if (policy.stop_requested())
        throw operation_cancelled{};
    // Braced initialization admits work left to right. The owning
    // coroutine keeps factories alive even if a later factory throws.
    auto deeds = std::apply(
        [&policy](auto &... work) {
            return deeds_type{
                policy.fork(start_firm_work(work)).cope()...};
        },
        work);
    co_await policy.join();
    co_return deeds;
}

template<typename, typename T>
using repeat_type = T;

template<typename T, typename Tuple, std::size_t... Is>
[[nodiscard]] T take_first_success_or_throw(
    Tuple & deeds,
    std::index_sequence<Is...>)
{
    auto exceptions = std::vector<std::exception_ptr>{};
    auto result = std::optional<T>{};

    auto inspect = [&](auto index) {
        if (result)
            return;

        auto value = std::move(std::get<index>(deeds)).get();
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
    Tuple & deeds,
    std::index_sequence<Is...>)
{
    auto exceptions = std::vector<std::exception_ptr>{};
    auto succeeded = false;

    auto inspect = [&](auto index) {
        if (succeeded)
            return;

        auto value = std::move(std::get<index>(deeds)).get();
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
[[nodiscard]] T take_deed_result(catching_deed<T> deed)
{
    auto value = std::move(deed).get();
    if (value)
        return std::move(*value);
    rethrow(value.error());
}

inline std::monostate take_deed_result(catching_deed<void> deed)
{
    auto value = std::move(deed).get();
    if (value)
        return {};
    rethrow(value.error());
}

template<typename Tuple, std::size_t... Is>
[[nodiscard]] auto take_all_or_throw(
    Tuple & deeds,
    std::index_sequence<Is...>)
{
    return std::tuple{
        take_deed_result(std::move(std::get<Is>(deeds)))...,
    };
}

} // namespace detail

/// Start tuple elements in an ordinary nursery.
/// Elements are tasks or owned nullary task factories. Factories run once,
/// under the new firm, and survive all child settlement (including
/// failure). Preconstructed tasks retain their original frame allocation;
/// both forms run under this firm's environment and may fork more children.
/// Returns settled catching deeds; the policy controls sibling cancellation,
/// not result extraction.
template<typename Policy = firm, typename... Work>
    requires std::derived_from<Policy, firm>
             && std::default_initializable<Policy>
             && ((is_task_v<Work> || stored_task_factory<Work>) && ...)
[[nodiscard]] task<
    std::tuple<catching_deed<detail::firm_work_result_t<Work>>...>>
with_firm(std::tuple<Work...> work)
{
    co_return co_await detail::make_firm_body<Policy>(
        [&work](Policy & policy) {
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
    auto deeds = co_await with_firm<stop_on_failure>(std::move(work));
    co_return detail::take_all_or_throw(
        deeds, std::index_sequence_for<Work...>{});
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
    auto deeds = co_await with_firm<stop_on_success>(std::move(work));
    if constexpr (std::is_void_v<result_type>) {
        detail::take_first_void_success_or_throw(
            deeds, std::index_sequence_for<First, Rest...>{});
    } else {
        co_return detail::take_first_success_or_throw<result_type>(
            deeds, std::index_sequence_for<First, Rest...>{});
    }
}

template<typename... Tasks>
    requires(sizeof...(Tasks) > 0) && (is_task_v<Tasks> && ...)
[[nodiscard]] task<std::tuple<task_result_t<Tasks>...>>
when_all(Tasks... tasks)
{
    using deeds_type = std::tuple<catching_deed<task_result_t<Tasks>>...>;
    constexpr auto count = sizeof...(Tasks);

    auto deeds = co_await detail::make_firm_body<stop_on_failure>(
        [... tasks = std::move(tasks)](
            auto & policy) mutable -> task<deeds_type> {
            auto deeds = deeds_type{
                policy.fork(std::move(tasks)).cope()...,
            };
            co_await policy.join();
            co_return deeds;
        });

    co_return detail::take_all_or_throw(
        deeds,
        std::make_index_sequence<count>{});
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

    auto deeds = co_await detail::make_firm_body<stop_on_failure>(
        [tasks = std::move(tasks)](auto & policy) mutable
            -> task<std::vector<deed_type>> {
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

    auto deeds = co_await detail::make_firm_body<stop_on_success>(
        [tasks = std::move(tasks)](auto & policy) mutable
            -> task<std::vector<deed_type>> {
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
    using deeds_type =
        std::tuple<
            catching_deed<T>,
            detail::repeat_type<Rest, catching_deed<T>>...>;
    constexpr auto count = std::size_t{1 + sizeof...(Rest)};

    auto deeds = co_await detail::make_firm_body<stop_on_success>(
        [first = std::move(first),
         ... rest = std::move(rest)](
            auto & policy) mutable -> task<deeds_type> {
            auto deeds = deeds_type{
                policy.fork(std::move(first)).cope(),
                policy.fork(std::move(rest)).cope()...,
            };
            co_await policy.join();
            co_return deeds;
        });

    if constexpr (std::is_void_v<T>) {
        detail::take_first_void_success_or_throw(
            deeds,
            std::make_index_sequence<count>{});
    } else {
        co_return detail::take_first_success_or_throw<T>(
            deeds,
            std::make_index_sequence<count>{});
    }
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
    using deeds_type =
        std::tuple<catching_deed<T>, catching_deed<void>>;

    auto deeds = co_await with_firm(
        [duration, body = std::move(body)]() mutable
            -> task<deeds_type> {
            // Parent cancellation can reach the scope before this body
            // gets its first turn. A stopped firm cannot accept forks.
            if (current_firm()->stop_requested())
                throw operation_cancelled{};
            auto body_deed =
                fork(detail::stop_firm_on_completion(std::move(body)))
                    .cope();
            auto timeout_deed =
                fork(detail::stop_firm_on_completion(
                    timeout_after(duration))).cope();
            auto deeds = deeds_type{
                std::move(body_deed),
                std::move(timeout_deed),
            };
            co_await join();
            co_return deeds;
        });

    auto body_result = std::move(std::get<0>(deeds)).get();
    if (body_result) {
        if constexpr (std::is_void_v<T>) {
            co_return;
        } else {
            co_return std::move(*body_result);
        }
    }

    auto timeout_result = std::move(std::get<1>(deeds)).get();
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
