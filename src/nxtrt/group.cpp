#include "nxtrt/task.hpp"

#include <cassert>

namespace nxtrt {

group::group([[maybe_unused]] group && other) noexcept
{
    // Coroutine parameters move before execution, never with live observers.
    assert(other.executor_ == nullptr);
}

group::~group() = default;

void group::detach() noexcept
{
    for (auto & child : children_) {
        if (child.promise && child.promise->completion == &child)
            child.promise->completion = nullptr;
        child.owner = nullptr;
    }
    children_ = {};
    executor_ = nullptr;
}

void group::stop() noexcept
{
    for (auto & child : children_)
        if (child.started && !child.completed)
            child.promise->request_stop();
}

void group::signal() noexcept
{
    if (pending_ == 0 && waiter_.handle)
        std::exchange(waiter_, {}).resume(*executor_);
}

void group::completed(detail::group_child & child) noexcept
{
    child.completed = true;
    if (child.started)
        --pending_;
    if (!stopped_ && should_stop(child.index, child.failed(child.promise)))
        stopped_ = true;
    if (stopped_)
        stop();
    signal();
}

struct group::stop_callback
{
    group & owner;
    void operator()() const noexcept
    {
        owner.parent_stopped_ = true;
        owner.stop();
    }
};

struct group::drain_awaiter
{
    group & owner;
    bool await_ready() noexcept
    {
        return owner.pending_ == 0;
    }
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        owner.waiter_ = need{handle, detail::current_env->current_promise};
    }
    void await_resume() noexcept {}
};

void detail::group_child::task_completed() noexcept
{
    owner->completed(*this);
}

task<void> group::run(std::span<detail::group_child> children)
{
    auto * env = detail::current_env;
    if (!env || !env->current_deck || !env->current_promise)
        throw runtime_error{"nxtrt group used without a running deck"};
    if (executor_)
        throw runtime_error{"nxtrt group is already running"};

    children_ = children;
    executor_ = env->current_deck;
    pending_ = 0;
    waiter_ = {};
    stopped_ = false;
    parent_stopped_ = false;
    // Disconnect observers before the caller destroys its child records,
    // including when this coroutine exits with an exception.
    struct binding
    {
        group & owner;
        ~binding() { owner.detach(); }
    } bound{*this};
    auto parent_stop = std::stop_callback{
        env->current_promise->stop_token(), stop_callback{*this}};
    auto setup_failure = std::exception_ptr{};
    try {
        // Validate even positions a stopping group might otherwise skip.
        for (auto & child : children)
            if (!child.handle)
                throw runtime_error{"nxtrt group received an empty task"};

        for (auto i = std::size_t{0}; i < children.size(); ++i) {
            auto & child = children[i];
            child.owner = this;
            child.index = i;
            if (child.handle.done()) {
                // Final suspension has already notified any old observer.
                completed(child);
                continue;
            }
            if (stopped_ || parent_stopped_)
                continue;
            child.promise->env.copy_entries_from(*env);
            child.promise->observe_completion_of(child);
            // enqueue may allocate. Count only successfully scheduled work.
            child.promise->enqueue_self(child.handle);
            child.started = true;
            ++pending_;
        }
    } catch (...) {
        setup_failure = std::current_exception();
        stop();
    }
    co_await drain_awaiter{*this};
    if (setup_failure)
        rethrow(setup_failure);
    if (parent_stopped_)
        throw operation_cancelled{};
}

} // namespace nxtrt
