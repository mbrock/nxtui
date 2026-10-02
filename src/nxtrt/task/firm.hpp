#pragma once

// Structured child ownership, forking, stopping, and joining.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/deed.hpp"
#include "nxtrt/task/frame_arena.hpp"

namespace nxtrt {

class firm
{
public:
    firm()
    {
        register_debug();
    }

    explicit firm(frame_storage_ref frames)
        : frames_(frames)
    {
        register_debug();
    }

private:
    void register_debug()
    {
        debug_id_ = debug::allocate_firm_id();
        debug::register_firm(
            debug::firm_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = children_.size(),
                .stopping = stopping_,
            });
    }

public:
    ~firm()
    {
        if (debug_id_ != 0)
            debug::unregister_firm(debug_id_);
    }

    firm(const firm &) = delete;
    firm & operator=(const firm &) = delete;
    firm(firm && other) noexcept
        : frames_(std::move(other.frames_))
        , children_(std::move(other.children_))
        , stop_(std::move(other.stop_))
        , debug_id_(std::exchange(other.debug_id_, 0))
        , debug_parent_(std::exchange(other.debug_parent_, 0))
        , stopping_(std::exchange(other.stopping_, false))
    {
        for_each_child([this](auto & child) {
            child.firm_record.owner = this;
        });
        debug_update();
    }
    firm & operator=(firm &&) = delete;

    [[nodiscard]] void * allocate_frame(std::size_t size)
    {
        if (auto * frame = frames_.allocate(size))
            return frame;
        throw_frame_arena_full(size);
    }

    [[nodiscard]] std::size_t frame_capacity() const noexcept
    {
        return frames_.capacity();
    }

    [[nodiscard]] std::size_t frame_high_water() const noexcept
    {
        return frames_.high_water();
    }

    [[nodiscard]] std::size_t frame_used() const noexcept
    {
        return frames_.used();
    }

    [[nodiscard]] const firm_frame_arena & frame_arena() const noexcept
    {
        return frames_;
    }

    [[nodiscard]] std::size_t child_count() const noexcept
    {
        return children_.size();
    }

    void stop() noexcept
    {
        stopping_ = true;
        stop_.request_stop();
        for_each_child([](auto & child) {
            child.request_stop();
        });
        debug_update();
    }

    [[nodiscard]] bool stopping() const noexcept
    {
        return stopping_;
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stop_.stop_requested();
    }

    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        return stop_.get_token();
    }

    template<typename T>
    deed<T> fork(task<T> child)
    {
        auto * current = detail::current_env;
        auto * active_deck =
            current == nullptr ? nullptr : current->current_deck;
        if (current == nullptr || active_deck == nullptr)
            throw runtime_error{
                "nxtrt firm fork used without a running deck"};
        if (stopping_)
            throw runtime_error{"nxtrt firm fork used after stop"};
        auto handle = child.release();
        if (!handle || handle.done())
            throw runtime_error{"nxtrt firm fork used with empty task"};

        auto result = deed<T>{std::in_place};
        auto record = std::unique_ptr<detail::child_record<T>>{};
        try {
            auto & promise = handle.promise();
            // Forked children outlive the call site, so they inherit the
            // current immutable environment snapshot.
            promise.env.copy_entries_from(*current);
            record = std::make_unique<detail::child_record<T>>(
                handle,
                *this,
                &result.state());
            // Grow before enqueueing: allocation failure must not leave
            // the deck pointing at a destroyed child.
            children_.emplace_back();
            promise.observe_completion_of(*record);
            try {
                record->firm_record.task =
                    active_deck->enqueue(handle, &promise);
            } catch (...) {
                children_.pop_back();
                throw;
            }
            result.state().record.child_task =
                record->firm_record.task;
        } catch (...) {
            if (!record)
                handle.destroy();
            throw;
        }

        children_.back() = std::move(record);
        debug_update();
        return result;
    }

    template<typename Fn, typename... Args>
        requires std::invocable<Fn, Args...>
            && is_task_v<std::invoke_result_t<Fn, Args...>>
    auto fork(Fn && fn, Args &&... args)
        -> deed<task_result_t<std::invoke_result_t<Fn, Args...>>>
    {
        return fork(std::invoke(
            std::forward<Fn>(fn),
            std::forward<Args>(args)...));
    }

    [[nodiscard]] task<void> join();

    [[nodiscard]] bool has_unjoined_children() const noexcept
    {
        for (auto const & child : children_) {
            if (!child->joined())
                return true;
        }
        return false;
    }

    [[nodiscard]] debug::firm_id debug_id() const noexcept
    {
        return debug_id_;
    }

    void debug_parent(debug::firm_id parent) noexcept
    {
        debug_parent_ = parent;
        debug_update();
    }

    // Shared policy boundary for nursery children and pool-owned work.
    // Notification neither transfers ownership nor retains a child record.
    virtual void completed(task_id, std::exception_ptr) noexcept {}

private:
    friend struct detail::child_record_base;

    void report_child_finished(
        detail::child_record_base & child,
        std::exception_ptr known_failure = {}) noexcept
    {
        if (child.firm_record.completion_reported)
            return;
        child.firm_record.completion_reported = true;
        auto failure =
            known_failure ? known_failure : child.completion_failure();
        completed(child.firm_record.task, failure);
    }

    void debug_update() const
    {
        debug::update_firm(
            debug::firm_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = children_.size(),
                .stopping = stopping_,
            });
    }

    template<typename Fn>
    void for_each_child(Fn && fn) noexcept
    {
        for (auto & child : children_)
            std::invoke(fn, *child);
    }

    [[noreturn]] void throw_frame_arena_full(std::size_t frame_size) const;

    firm_frame_arena frames_;
    // Records never move: promises and deeds link directly to them.
    // Retain settlement records until nursery destruction, even after
    // their frames have been evacuated and returned to the frame pool.
    std::vector<std::unique_ptr<detail::child_record_base>> children_;
    std::stop_source stop_;
    debug::firm_id debug_id_ = 0;
    debug::firm_id debug_parent_ = 0;
    bool stopping_ = false;
};

struct firm_key
{
    using value_type = firm *;
    static constexpr auto name = "firm";
};

inline firm * current_firm() noexcept
{
    auto value = env_get<firm_key>();
    if (!value)
        return nullptr;
    return *value;
}

inline firm & require_current_firm()
{
    auto * firm = current_firm();
    if (firm == nullptr)
        throw runtime_error{"nxtrt operation used without firm"};
    return *firm;
}
inline void firm::throw_frame_arena_full(std::size_t frame_size) const
{
    auto message = std::string{"nxtrt firm frame arena is full: firm "};
    message += std::to_string(debug_id_);
    message += " needs ";
    message += std::to_string(firm_frame_arena::block_size(frame_size));
    message += " bytes for a ";
    message += std::to_string(frame_size);
    message += "-byte frame (alignment ";
    message += std::to_string(firm_frame_arena::block_alignment);
    message += "); ";
    message += std::to_string(frames_.used());
    message += " of ";
    message += std::to_string(frames_.capacity());
    message += " bytes held by ";
    message += std::to_string(frames_.live_frames());
    message += " live frames, top ";
    message += std::to_string(frames_.top());
    message += ", high water ";
    message += std::to_string(frames_.high_water());
    message += ", ";
    message += std::to_string(frames_.free_listed_bytes());
    message += " bytes free-listed, ";
    message += std::to_string(frames_.stranded_bytes());
    message += " stranded";
    if (auto * env = current_env();
        env != nullptr && env->current_promise != nullptr) {
        if (auto id = env->current_promise->id) {
            message += "; created by task ";
            message += std::to_string(id.index());
            message += ".";
            message += std::to_string(id.era());
        }
    }
    throw runtime_error{std::move(message)};
}

[[nodiscard]] inline task<void> join()
{
    co_await require_current_firm().join();
}

inline deck * current_deck() noexcept
{
    auto * env = current_env();
    return env == nullptr ? nullptr : env->current_deck;
}

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
    auto * firm = current_firm();
    if (firm == nullptr)
        return current_task_stop_token();
    return firm->stop_token();
}

inline bool stop_requested() noexcept
{
    auto * firm = current_firm();
    return task_stop_requested()
        || (firm != nullptr && firm->stop_requested());
}

inline void throw_if_stop_requested()
{
    if (stop_requested())
        throw operation_cancelled{};
}

template<typename T>
deed<T> fork(task<T> child)
{
    return require_current_firm().fork(std::move(child));
}

template<typename Fn, typename... Args>
    requires std::invocable<Fn, Args...>
        && is_task_v<std::invoke_result_t<Fn, Args...>>
auto fork(Fn && fn, Args &&... args)
    -> deed<task_result_t<std::invoke_result_t<Fn, Args...>>>
{
    return require_current_firm().fork(
        std::forward<Fn>(fn),
        std::forward<Args>(args)...);
}
inline task<void> firm::join()
{
    auto failures = std::vector<std::exception_ptr>{};
    for (auto i = std::size_t{0}; i < children_.size(); ++i) {
        // A child may fork more children while join is suspended.
        // Keep the stable record pointer, not a vector element reference.
        auto * record = children_[i].get();
        auto failure = std::exception_ptr{};
        auto collect_failure = std::exception_ptr{};
        try {
            auto & child = *record;
            co_await child.join();
            failure = child.failure();
            report_child_finished(child, failure);
            auto const exported = child.result_exported();
            if (!child.result_contained()
                && !child.result_observed()
                && !exported) {
                if (failure
                    && !(stop_requested()
                         && is_operation_cancelled(failure)))
                    collect_failure = failure;
            }
        } catch (...) {
            failure = std::current_exception();
            report_child_finished(*record, failure);
            if (!(stop_requested() && is_operation_cancelled(failure)))
                collect_failure = failure;
        }
        if (collect_failure)
            failures.push_back(std::move(collect_failure));
    }

    if (!failures.empty())
        throw_exceptions("firm tasks failed", std::move(failures));
}

} // namespace nxtrt
