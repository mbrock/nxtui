#include "nxtrt/task.hpp"

namespace nxtrt::detail {

struct group_coordinator
{
    std::span<group_child> children;
    group_control & control;
    deck & executor;
    std::size_t pending = 0;
    need waiter{};
    bool parent_stopped = false;

    ~group_coordinator()
    {
        for (auto & child : children) {
            if (child.promise && child.promise->completion == &child)
                child.promise->completion = nullptr;
            child.owner = nullptr;
        }
    }

    void stop() noexcept
    {
        for (auto & child : children)
            if (child.started && !child.completed)
                child.promise->request_stop();
    }

    void signal() noexcept
    {
        if (pending == 0 && waiter.handle)
            std::exchange(waiter, {}).resume(executor);
    }

    void completed(group_child & child) noexcept
    {
        child.completed = true;
        if (child.started)
            --pending;
        control.settled(child.index, child.failed(child.promise));
        if (control.stopped)
            stop();
        signal();
    }

    struct stop_callback
    {
        group_coordinator & owner;
        void operator()() const noexcept
        {
            owner.parent_stopped = true;
            owner.stop();
        }
    };

    struct drain_awaiter
    {
        group_coordinator & owner;
        bool await_ready() noexcept
        {
            return owner.pending == 0;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept
        {
            owner.waiter = need{handle, current_env->current_promise};
        }
        void await_resume() noexcept {}
    };
};

void group_child::task_completed() noexcept
{
    owner->completed(*this);
}

task<void>
run_group(std::span<group_child> children, group_control & control)
{
    auto * env = current_env;
    if (!env || !env->current_deck || !env->current_promise)
        throw runtime_error{"nxtrt group used without a running deck"};
    auto coordinator = group_coordinator{children, control, *env->current_deck};
    auto parent_stop = std::stop_callback{
        env->current_promise->stop_token(),
        group_coordinator::stop_callback{coordinator}};
    auto setup_failure = std::exception_ptr{};
    try {
        // Validate even positions a stop rule might otherwise skip.
        for (auto & child : children)
            if (!child.handle)
                throw runtime_error{"nxtrt group received an empty task"};

        for (auto i = std::size_t{0}; i < children.size(); ++i) {
            auto & child = children[i];
            child.owner = &coordinator;
            child.index = i;
            if (child.handle.done()) {
                // Final suspension has already notified any old observer.
                coordinator.completed(child);
                continue;
            }
            if (control.stopped || coordinator.parent_stopped)
                continue;
            child.promise->env.copy_entries_from(*env);
            child.promise->observe_completion_of(child);
            // enqueue may allocate. Count only successfully scheduled work.
            child.promise->enqueue_self(child.handle);
            child.started = true;
            ++coordinator.pending;
        }
    } catch (...) {
        setup_failure = std::current_exception();
        coordinator.stop();
    }
    co_await group_coordinator::drain_awaiter{coordinator};
    if (setup_failure)
        rethrow(setup_failure);
    if (coordinator.parent_stopped)
        throw operation_cancelled{};
}

} // namespace nxtrt::detail
