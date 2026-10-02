#pragma once

// Definitions connecting completed promise, child, firm, deck, and wish types.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/firm.hpp"

namespace nxtrt {

namespace detail {

inline void child_record_base::task_completed() noexcept
{
    if (firm_record.owner != nullptr)
        firm_record.owner->report_child_finished(*this);
}

inline void * allocate_task_frame(std::size_t size)
{
    if (auto * firm = current_firm()) {
        auto * ptr = firm->allocate_frame(size);
        alloc_trace::event(
            "task-frame",
            "new",
            ptr,
            size,
            alignof(task_frame_header),
            firm->frame_used(),
            firm->frame_capacity());
        return ptr;
    }

    throw runtime_error{
        "nxtrt task created without current firm (root tasks must be "
        "created inside runtime::run or deck::sync_wait: pass a task "
        "factory)"};
}

inline void deallocate_task_frame(void * ptr, std::size_t size) noexcept
{
    if (ptr == nullptr)
        return;

    auto & arena = firm_frame_arena::owner_of(ptr);
    arena.deallocate(ptr);
    alloc_trace::event(
        "task-frame",
        "del",
        ptr,
        size,
        alignof(task_frame_header),
        arena.used(),
        arena.capacity());
}

} // namespace detail

inline task_id deck::current_task_id() const noexcept
{
    auto * env = current_env();
    if (env == nullptr
        || env->current_deck != this
        || env->current_promise == nullptr)
        return {};
    return env->current_promise->id;
}

inline void deck::dump_if_requested()
{
    if (!debug::consume_signal_dump_request())
        return;

    std::cerr << "\n" << runtime_dump_text() << std::flush;
}

inline std::string deck::runtime_dump_text() const
{
    auto ready = std::vector<task_id>{};
    ready.reserve(ready_.size());
    for (auto id : ready_)
        ready.push_back(id);

    return debug::format_runtime_dump(
        debug::snapshot_firms(),
        debug::snapshot_waits(),
        std::move(ready));
}

inline task_id deck::register_task(
    std::coroutine_handle<> handle,
    detail::promise_base * promise)
{
    if (!handle || promise == nullptr)
        return {};

    if (promise->registered_deck != nullptr
        && promise->registered_deck != this)
        throw runtime_error{"nxtrt task scheduled on a different deck"};

    if (auto id = promise->id) {
        if (auto * existing = resolve(id)) {
            if (existing->promise != promise)
                throw runtime_error{"nxtrt task id collision"};
            existing->handle = handle;
            return id;
        }
        promise->id = {};
    }

    for (auto i = std::size_t{0}; i < tasks_.size(); ++i) {
        auto & row = tasks_[i];
        if (row.state != deck_task_state::vacant)
            continue;

        auto index = i + 1;
        if (index > task_id::max_index)
            throw runtime_error{"nxtrt deck task table is too large"};

        row.id = task_id::make(
            static_cast<std::uint32_t>(index),
            row.era);
        row.handle = handle;
        row.promise = promise;
        row.state = deck_task_state::live;
        promise->id = row.id;
        promise->registered_deck = this;
        return row.id;
    }

    throw runtime_error{"nxtrt deck task table is full"};
}

inline deck_task_record * deck::resolve(task_id id) noexcept
{
    if (!id)
        return nullptr;
    auto index = id.index();
    if (index == 0 || index > tasks_.size())
        return nullptr;
    auto & row = tasks_[index - 1];
    if (row.state != deck_task_state::live || row.id != id)
        return nullptr;
    return &row;
}

inline const deck_task_record * deck::resolve(task_id id) const noexcept
{
    if (!id)
        return nullptr;
    auto index = id.index();
    if (index == 0 || index > tasks_.size())
        return nullptr;
    auto const & row = tasks_[index - 1];
    if (row.state != deck_task_state::live || row.id != id)
        return nullptr;
    return &row;
}

inline void deck::unregister_task(
    task_id id,
    detail::promise_base * promise) noexcept
{
    auto * row = resolve(id);
    if (row == nullptr || row->promise != promise)
        return;

    row->id = {};
    row->handle = {};
    row->promise = nullptr;
    row->state = deck_task_state::vacant;
    ++row->era;
    if (row->era == 0)
        row->era = 1;

    if (promise != nullptr) {
        promise->id = {};
        if (promise->registered_deck == this)
            promise->registered_deck = nullptr;
    }
}

inline task_id deck::enqueue(
    std::coroutine_handle<> handle,
    detail::promise_base * promise)
{
    auto id = register_task(handle, promise);
    if (!id)
        return {};
    trace("deck enqueue task {}", id.value);
    ready_.push_back(id);
    return id;
}

inline void deck::resume_if_ready(task_id id)
{
    auto * task = resolve(id);
    if (task == nullptr)
        return;

    auto handle = task->handle;
    auto * promise = task->promise;
    if (!handle || handle.done())
        return;

    trace("deck resume task {}", id.value);
    auto env_guard = detail::env_guard{promise->env, this, promise};
    handle.resume();
}

inline void detail::promise_base::unregister_from_deck() noexcept
{
    if (registered_deck == nullptr)
        return;
    registered_deck->unregister_task(id, this);
}

inline void need::resume(deck & d) const
{
    trace("wand fulfill parked task");
    if (promise != nullptr)
        promise->clear_wait_stop_callback();
    if (promise != nullptr)
        debug::unpark_task(promise->id);
    d.enqueue(handle, promise);
}

namespace detail {

struct running_wish_context
{
    deck * active_deck = nullptr;
    wand * active_wand = nullptr;
    promise_base * running = nullptr;
};

inline running_wish_context current_wish_context() noexcept
{
    auto * current = detail::current_env;
    auto * active_deck = current == nullptr ? nullptr : current->current_deck;
    return running_wish_context{
        .active_deck = active_deck,
        .active_wand =
            active_deck == nullptr ? nullptr : active_deck->current_wand(),
        .running = current == nullptr ? nullptr : current->current_promise,
    };
}

template<typename Wish>
auto prep_wish_awaitable(Wish const & wish)
{
    auto context = current_wish_context();
    if (context.active_deck == nullptr
        || context.active_wand == nullptr
        || context.running == nullptr) {
        auto message = std::string{"nxtrt "};
        message.append(Wish::name);
        message.append(" wish awaited without a running wand");
        throw runtime_error{std::move(message)};
    }

    op::trace_wish(wish);
    return context.active_wand->prepare(
        *context.active_deck,
        *context.running,
        wish);
}

} // namespace detail

template<typename T>
inline void urge<T>::await_suspend(
    std::coroutine_handle<> awaiting) const
{
    auto * active_wand = source_;
    auto * current = detail::current_env;
    auto * running = current == nullptr ? nullptr : current->current_promise;
    if (active_wand == nullptr || running == nullptr)
        throw runtime_error{
            "nxtrt urge awaited without a prepared wand"};

    trace("urge suspend token={}", coin_);
    debug::park_task(
        running->id,
        coin_,
        debug::parked_wish_description(description_));
    active_wand->suspend(
        coin_,
        need{
            .handle = awaiting,
            .promise = running,
        });
    running->cancel_wait_on_stop(*active_wand, coin_);
}

namespace op {

template<awaitable_wish Wish>
inline urge<typename Wish::result_type> operator co_await(Wish const & wish)
{
    return detail::prep_wish_awaitable(wish);
}

} // namespace op

struct yield_awaiter
{
    /// Yielding always suspends so the coroutine returns to the pump.
    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    /// Re-enqueue the currently running coroutine.
    void await_suspend(std::coroutine_handle<> awaiting) const
    {
        auto * current = detail::current_env;
        auto * active_deck =
            current == nullptr ? nullptr : current->current_deck;
        auto * running = current == nullptr ? nullptr : current->current_promise;
        if (active_deck == nullptr || running == nullptr)
            throw runtime_error{
                "nxtrt yield awaited without a running deck"};
        active_deck->enqueue(awaiting, running);
    }

    /// No value is produced by `co_await nxtrt::yield()`.
    void await_resume() const noexcept {}
};

inline yield_awaiter yield() noexcept
{
    return yield_awaiter{};
}

template<typename T>
inline void deck::start(task<T> & t)
{
    auto handle = t.handle();
    if (!handle || handle.done())
        return;
    enqueue(handle, &handle.promise());
}

} // namespace nxtrt
