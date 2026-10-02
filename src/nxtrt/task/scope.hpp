#pragma once

// Firm body execution, environment binding, and lifetime enforcement.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/context.hpp"
#include "nxtrt/task/firm.hpp"
#include "nxtrt/task/runtime.hpp"

namespace nxtrt {

namespace detail {

template<typename T>
class owning_task_awaiter
{
public:
    explicit owning_task_awaiter(task<T> child)
        : child_(std::move(child))
        , inner_(child_.operator co_await())
    {}

    [[nodiscard]] bool await_ready()
    {
        return inner_.await_ready();
    }

    void await_suspend(std::coroutine_handle<> awaiting)
    {
        inner_.await_suspend(awaiting);
    }

    decltype(auto) await_resume()
    {
        return inner_.await_resume();
    }

private:
    task<T> child_;
    decltype(std::declval<task<T> &>().operator co_await()) inner_;
};

[[nodiscard]] inline std::exception_ptr unjoined_firm_children_error()
{
    try {
        throw runtime_error{
            "nxtrt firm body returned with unjoined children; "
            "co_await scope.join() inside the firm body "
            "before locals captured by forked children go out of scope"};
    } catch (...) {
        return std::current_exception();
    }
}

template<stored_task_factory Fn>
[[nodiscard]] task<stored_task_result_t<Fn>>
run_firm_body(firm & firm, Fn fn)
{
    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        auto body_failure = std::exception_ptr{};
        try {
            co_await std::invoke(fn);
        } catch (...) {
            body_failure = std::current_exception();
        }
        if (!body_failure && firm.has_unjoined_children())
            body_failure = unjoined_firm_children_error();

        if (!body_failure)
            co_return;

        auto exceptions = std::vector<std::exception_ptr>{body_failure};
        firm.stop();
        try {
            co_await firm.join();
        } catch (...) {
            exceptions.push_back(std::current_exception());
        }
        throw_exceptions("firm body failed", std::move(exceptions));
    } else {
        using result_type = std::remove_cv_t<stored_task_result_t<Fn>>;
        auto result = std::optional<result_type>{};
        auto body_failure = std::exception_ptr{};
        try {
            result.emplace(co_await std::invoke(fn));
        } catch (...) {
            body_failure = std::current_exception();
        }
        if (!body_failure && firm.has_unjoined_children())
            body_failure = unjoined_firm_children_error();
        if (!body_failure)
            co_return std::move(*result);

        auto exceptions = std::vector<std::exception_ptr>{body_failure};
        firm.stop();
        try {
            co_await firm.join();
        } catch (...) {
            exceptions.push_back(std::current_exception());
        }
        throw_exceptions("firm body failed", std::move(exceptions));
    }
}

template<typename Base, typename Fn>
class firm_body : public Base
{
public:
    explicit firm_body(Fn fn)
        : fn_(std::move(fn))
    {
    }

    auto operator()()
    {
        if constexpr (stored_task_factory<Fn, Base &>)
            return std::invoke(fn_, static_cast<Base &>(*this));
        else
            return std::invoke(fn_);
    }

private:
    Fn fn_;
};

template<typename Firm>
struct firm_invoker
{
    Firm * firm = nullptr;

    auto operator()() const
    {
        return std::invoke(*firm);
    }
};

} // namespace detail

template<typename Firm>
    requires std::derived_from<Firm, firm>
        && stored_task_factory<Firm>
[[nodiscard]] task<stored_task_result_t<Firm>>
run_firm(Firm firm_scope)
{
    firm_scope.parent_ = current_firm();
    firm_scope.debug_update();
    auto body = detail::run_firm_body(
        firm_scope,
        detail::firm_invoker<Firm>{&firm_scope});
    auto stop_firm = [&firm_scope] {
        firm_scope.stop();
    };
    auto stop_firm_callback =
        std::stop_callback{current_task_stop_token(), stop_firm};

    auto run_bound = [body = std::move(body)]() mutable {
        return std::move(body);
    };

    if constexpr (std::is_void_v<stored_task_result_t<Firm>>) {
        co_await with_env<firm_key>(&firm_scope, std::move(run_bound));
    } else {
        co_return co_await with_env<firm_key>(
            &firm_scope,
            std::move(run_bound));
    }
}

template<typename Firm>
    requires std::derived_from<Firm, firm>
        && stored_task_factory<Firm>
[[nodiscard]] auto operator co_await(Firm firm_scope)
{
    using result_type = stored_task_result_t<Firm>;
    return detail::owning_task_awaiter<result_type>{
        run_firm(std::move(firm_scope))};
}

/// Establish frame/cancellation context. A body that needs child ownership
/// receives the scope explicitly; nullary bodies need no spawning
/// capability.
template<typename Policy = firm, typename Fn>
    requires std::derived_from<Policy, firm>
             && std::default_initializable<Policy>
             && (stored_task_factory<std::decay_t<Fn>, Policy &>
                 || stored_task_factory<std::decay_t<Fn>>)
[[nodiscard]] auto with_firm(Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    return run_firm(
        detail::firm_body<Policy, factory_type>{
            factory_type{std::forward<Fn>(fn)}});
}

} // namespace nxtrt
