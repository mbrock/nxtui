#pragma once

// Root task ownership and synchronous deck entry.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/scope.hpp"

namespace nxtrt {

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
        [[maybe_unused]] auto previous_root_firm =
            env_.replace<firm_key>(&firm_);
        auto root_guard = detail::env_guard{env_, &d, nullptr};
        task_ = std::invoke(factory_type{std::forward<Fn>(fn)});
    }

    void start()
    {
        deck_->start(task_);
    }

    [[nodiscard]] task<T> & inner() noexcept
    {
        return task_;
    }

    [[nodiscard]] const task<T> & inner() const noexcept
    {
        return task_;
    }

    [[nodiscard]] firm & root_firm() noexcept
    {
        return firm_;
    }

private:
    firm firm_;
    runtime_env env_;
    task<T> task_;
    deck * deck_ = nullptr;
};

template<typename Fn>
root_task(deck &, Fn &&)
    -> root_task<stored_task_result_t<std::decay_t<Fn>>>;

template<task_factory Fn>
[[nodiscard]] task_result_t<std::invoke_result_t<Fn>>
deck::sync_wait(Fn && fn)
{
    using factory_type = std::decay_t<Fn>;

    auto root_firm = firm{};
    auto root_env = runtime_env{};
    [[maybe_unused]] auto previous_root_firm =
        root_env.replace<firm_key>(&root_firm);
    auto root_guard = detail::env_guard{root_env, this, nullptr};

    return drive(with_firm(factory_type{std::forward<Fn>(fn)}));
}

} // namespace nxtrt
