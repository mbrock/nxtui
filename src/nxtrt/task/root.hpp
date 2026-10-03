#pragma once

// Root task ownership and synchronous deck entry.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/context.hpp"

namespace nxtrt {

/// A top-level task created in its own root environment, for hosts that
/// drive a deck themselves instead of calling `deck::sync_wait`.
///
/// The constructor calls `fn()` once, with `d` as the current deck, so the
/// new task captures a fresh, empty env. `fn` is a temporary: it is invoked
/// and then dropped, so it must return a task that does not refer to the
/// closure itself. Use a plain lambda that calls a task function
/// (`[&] { return serve(args); }`), not a capturing coroutine lambda.
///
/// `start()` queues the task on the deck; the host then pumps the deck (and
/// usually its wand, for example with a wand's `run_until_done`) and reads
/// the result through `inner()`. The root task is pinned in place and owns
/// the task; stop it with `inner().request_stop()`.
///
/// @code
/// auto root = nxtrt::root_task{deck, [&] { return serve(port); }};
/// root.start();
/// wand.run_until_done(deck, root.inner());
/// std::move(root.inner()).result();
/// @endcode
template<typename T>
class root_task
{
public:
    root_task() = delete;
    root_task(const root_task &) = delete;
    root_task & operator=(const root_task &) = delete;
    root_task(root_task &&) = delete;
    root_task & operator=(root_task &&) = delete;

    template<typename Fn>
        requires stored_task_factory<std::decay_t<Fn>>
            && std::same_as<
                stored_task_result_t<std::decay_t<Fn>>,
                T>
    root_task(deck & d, Fn && fn)
        : deck_(&d)
    {
        using factory_type = std::decay_t<Fn>;
        auto root_guard = detail::env_guard{env_, &d, nullptr};
        task_ = std::invoke(factory_type{std::forward<Fn>(fn)});
    }

    /// Queue the task on its deck. Does nothing if it is already done.
    void start()
    {
        deck_->start(task_);
    }

    /// The owned task, for `done()`, `result()` and `request_stop()`.
    [[nodiscard]] task<T> & inner() noexcept
    {
        return task_;
    }

    [[nodiscard]] const task<T> & inner() const noexcept
    {
        return task_;
    }

private:
    runtime_env env_;
    task<T> task_;
    deck * deck_ = nullptr;
};

template<typename Fn>
root_task(deck &, Fn &&)
    -> root_task<stored_task_result_t<std::decay_t<Fn>>>;

template<typename Fn, typename... Args>
    requires task_factory<std::decay_t<Fn> &, Args...>
[[nodiscard]] task_result_t<std::invoke_result_t<std::decay_t<Fn> &, Args...>>
deck::sync_wait(Fn && fn, Args &&... args)
{
    // The factory outlives its task: a capturing coroutine lambda's frame
    // refers to the closure object.
    auto factory = std::decay_t<Fn>{std::forward<Fn>(fn)};
    auto root_env = runtime_env{};
    auto root_guard = detail::env_guard{root_env, this, nullptr};

    return drive(std::invoke(factory, std::forward<Args>(args)...));
}

} // namespace nxtrt
