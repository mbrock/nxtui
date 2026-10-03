#pragma once

// Ambient deck and stop state, task-local environment bindings, and trace
// spans. Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/task.hpp"

namespace nxtrt {

/// The deck that is resuming the current coroutine on this thread, or null
/// outside deck execution. Inside `deck::sync_wait`'s task factory it is
/// that deck.
inline deck * current_deck() noexcept
{
    auto * env = current_env();
    return env == nullptr ? nullptr : env->current_deck;
}

/// The running task's stop token, or an empty token (no stop possible)
/// outside a running task.
///
/// Stop propagates from an awaiting task to the task it awaits (unless the
/// child is shielded), and a group stops its own jobs according to its
/// policy. Same as `current_stop_token()`.
inline std::stop_token current_task_stop_token() noexcept
{
    auto * env = current_env();
    if (env == nullptr || env->current_promise == nullptr)
        return {};
    return env->current_promise->stop_token();
}

/// True if the running task has been asked to stop; false outside a
/// running task. Same as `stop_requested()`.
inline bool task_stop_requested() noexcept
{
    auto * env = current_env();
    return env != nullptr
        && env->current_promise != nullptr
        && env->current_promise->stop_requested();
}

/// The running task's stop token; see `current_task_stop_token()`. Pass it
/// to code that takes a `std::stop_token`, or register a
/// `std::stop_callback` on it.
inline std::stop_token current_stop_token() noexcept
{
    return current_task_stop_token();
}

/// True if the running task has been asked to stop. Stop is cooperative:
/// parked wishes are cancelled for you, but CPU-bound loops should check
/// this (or call `throw_if_stop_requested()`) between steps.
inline bool stop_requested() noexcept
{
    return task_stop_requested();
}

/// Throw `operation_cancelled` if the running task has been asked to stop.
inline void throw_if_stop_requested()
{
    if (stop_requested())
        throw operation_cancelled{};
}

namespace detail {

/// Coroutine behind `with_env`. Binds `Key` in this coroutine's own
/// environment, creates and awaits the child from `fn` (which inherits the
/// binding), and restores the previous snapshot when it finishes.
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

/// A task that runs the task made by `fn()` with the env key `Key` bound to
/// `value`, and returns its result.
///
/// `Key` is an env key type (see @ref nxtrt::runtime_env "runtime_env").
/// The binding is visible to the task `fn` creates and to everything that
/// task awaits or starts in groups, through `env_get<Key>()` and
/// `env_require<Key>()`. Code outside the returned task, including the
/// caller, never sees it. Nested `with_env` calls for the same key shadow
/// the outer value until they finish.
///
/// `fn` is copied or moved into the returned task's frame and invoked once
/// when that task runs, so it may be a capturing coroutine lambda. Lazy
/// like any task; throws `runtime_error` if it runs outside a deck task.
///
/// @code
/// struct request_id_key
/// {
///     using value_type = int;
///     static constexpr auto name = "request-id";
/// };
///
/// auto id = co_await nxtrt::with_env<request_id_key>(
///     7, [] { return handle_request(); });
/// @endcode
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

namespace detail {

template<stored_task_factory Fn>
[[nodiscard]] task<stored_task_result_t<Fn>>
with_trace_span_task(std::string name, trace_attributes attributes, Fn fn)
{
    auto context = current_trace_context();
    if (context == nullptr) {
        auto child = std::invoke(fn);
        if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
            co_await child;
        } else {
            co_return co_await child;
        }
    } else {
        auto span = context->start_span(
            std::move(name), current_trace_span_id(), std::move(attributes));
        try {
            if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
                co_await with_env<trace_current_span_key>(
                    span.span_id(), [&]() -> task<void> {
                    co_await std::invoke(fn);
                });
                span.finish("ok");
            } else {
                auto result = co_await with_env<trace_current_span_key>(
                    span.span_id(),
                    [&]() -> task<stored_task_result_t<Fn>> {
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

} // namespace detail

/// A task that runs the task made by `fn()` inside a new trace span.
///
/// If a trace context is bound in the env (`trace_context_key`), this
/// starts a span named `name` with `attributes` as a child of the current
/// span, binds it as the current span while `fn`'s task runs, and finishes
/// it with status `"ok"` or, if the task throws, `"error"` (the exception
/// propagates). Without a trace context it just runs `fn`'s task.
///
/// `fn` is copied or moved into the returned task's frame and stays alive
/// until it finishes, so the lazy task may be stored before it is awaited.
template<typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] task<stored_task_result_t<std::decay_t<Fn>>>
with_trace_span(std::string name, trace_attributes attributes, Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    return detail::with_trace_span_task(
        std::move(name),
        std::move(attributes),
        factory_type{std::forward<Fn>(fn)});
}

/// `with_trace_span` with no span attributes.
template<typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] task<stored_task_result_t<std::decay_t<Fn>>>
with_trace_span(std::string name, Fn && fn)
{
    return with_trace_span(
        std::move(name), {}, std::forward<Fn>(fn));
}

} // namespace nxtrt
