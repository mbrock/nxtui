#pragma once

// Ambient deck and stop state, task-local environment bindings, and trace
// spans. Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/task.hpp"

namespace nxtrt {

inline deck * current_deck() noexcept
{
    auto * env = current_env();
    return env == nullptr ? nullptr : env->current_deck;
}

/// The running task's stop token. Stop propagates from an awaiting task to
/// the task it awaits, and a group stops its own jobs.
inline std::stop_token current_task_stop_token() noexcept
{
    auto * env = current_env();
    if (env == nullptr || env->current_promise == nullptr)
        return {};
    return env->current_promise->stop_token();
}

inline bool task_stop_requested() noexcept
{
    auto * env = current_env();
    return env != nullptr
        && env->current_promise != nullptr
        && env->current_promise->stop_requested();
}

inline std::stop_token current_stop_token() noexcept
{
    return current_task_stop_token();
}

inline bool stop_requested() noexcept
{
    return task_stop_requested();
}

inline void throw_if_stop_requested()
{
    if (stop_requested())
        throw operation_cancelled{};
}

namespace detail {

/// Run `fn` with `Key` temporarily bound in the current task environment.
///
/// The binding mutates the promise-owned environment and restores the previous
/// entry when the scoped child task completes.
template<typename Key, stored_task_factory Fn>
[[nodiscard]] task<stored_task_result_t<Fn>>
with_env_bound(typename Key::value_type value, Fn fn)
{
    auto * current = detail::current_env;
    auto * promise = current == nullptr ? nullptr : current->current_promise;
    if (current == nullptr || promise == nullptr)
        throw runtime_error{
            "nxtrt env binding used without runtime env"};

    struct binding_guard
    {
        detail::promise_base * promise = nullptr;
        runtime_env::entry_snapshot previous;

        ~binding_guard() noexcept
        {
            if (promise != nullptr)
                promise->env.restore(std::move(previous));
        }
    };

    auto restore = binding_guard{
        .promise = promise,
        .previous = promise->env.template replace<Key>(std::move(value)),
    };
    auto child = std::invoke(fn);

    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        co_await child;
    } else {
        co_return co_await child;
    }
}

} // namespace detail

template<typename Key, typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] task<stored_task_result_t<std::decay_t<Fn>>>
with_env(typename Key::value_type value, Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    return detail::with_env_bound<Key>(
        std::move(value),
        factory_type{std::forward<Fn>(fn)});
}

template<task_factory Fn>
[[nodiscard]] task<task_result_t<std::invoke_result_t<Fn>>>
with_trace_span(std::string name, trace_attributes attributes, Fn && fn)
{
    auto context = current_trace_context();
    if (context == nullptr) {
        auto child = std::invoke(std::forward<Fn>(fn));
        if constexpr (std::is_void_v<task_result_t<std::invoke_result_t<Fn>>>) {
            co_await child;
        } else {
            co_return co_await child;
        }
    } else {
        auto span = context->start_span(
            std::move(name), current_trace_span_id(), std::move(attributes));
        try {
            if constexpr (
                std::is_void_v<task_result_t<std::invoke_result_t<Fn>>>) {
                co_await with_env<trace_current_span_key>(
                    span.span_id(), [&]() -> task<void> {
                    co_await std::invoke(fn);
                });
                span.finish("ok");
            } else {
                auto result = co_await with_env<trace_current_span_key>(
                    span.span_id(),
                    [&]() -> task<task_result_t<std::invoke_result_t<Fn>>> {
                    co_return co_await std::invoke(fn);
                });
                span.finish("ok");
                co_return std::move(result);
            }
        } catch (...) {
            span.finish("error");
            throw;
        }
    }
}

template<task_factory Fn>
[[nodiscard]] task<task_result_t<std::invoke_result_t<Fn>>>
with_trace_span(std::string name, Fn && fn)
{
    return with_trace_span(
        std::move(name), {}, std::forward<Fn>(fn));
}

} // namespace nxtrt
