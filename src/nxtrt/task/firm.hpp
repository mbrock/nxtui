#pragma once

// Structured child ownership, forking, stopping, and joining.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/storage.hpp"

namespace nxtrt {

class firm
{
public:
    static constexpr std::size_t default_child_capacity = 4096;

    firm()
        : owned_child_storage_(default_child_capacity)
        , child_slots_(owned_child_storage_.ref().slots)
        , uses_owned_child_storage_(true)
        , owned_deed_storage_(default_child_capacity)
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(default_child_capacity)
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(default_child_capacity)
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    explicit firm(frame_storage_ref frames)
        : frames_(frames)
        , owned_child_storage_(default_child_capacity)
        , child_slots_(owned_child_storage_.ref().slots)
        , uses_owned_child_storage_(true)
        , owned_deed_storage_(default_child_capacity)
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(default_child_capacity)
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(default_child_capacity)
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    explicit firm(firm_child_storage_ref children)
        : child_slots_(children.slots)
        , owned_deed_storage_(children.slots.size())
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(children.slots.size())
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    firm(
        firm_child_storage_ref children,
        firm_deed_storage_ref deeds)
        : child_slots_(children.slots)
        , deed_records_(deeds.records)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(children.slots.size())
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    explicit firm(firm_bookkeeping_storage_ref storage)
        : child_slots_(storage.children.slots)
        , deed_records_(storage.deeds.records)
        , completion_slots_(storage.completions.completions)
        , join_failure_slots_(storage.joins.failures)
    {
        register_debug();
    }

    firm(frame_storage_ref frames, firm_child_storage_ref children)
        : frames_(frames)
        , child_slots_(children.slots)
        , owned_deed_storage_(children.slots.size())
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(children.slots.size())
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    firm(
        frame_storage_ref frames,
        firm_child_storage_ref children,
        firm_deed_storage_ref deeds)
        : frames_(frames)
        , child_slots_(children.slots)
        , deed_records_(deeds.records)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , owned_join_storage_(children.slots.size())
        , join_failure_slots_(owned_join_storage_.ref().failures)
        , uses_owned_join_storage_(true)
    {
        register_debug();
    }

    firm(frame_storage_ref frames, firm_bookkeeping_storage_ref storage)
        : frames_(frames)
        , child_slots_(storage.children.slots)
        , deed_records_(storage.deeds.records)
        , completion_slots_(storage.completions.completions)
        , join_failure_slots_(storage.joins.failures)
    {
        register_debug();
    }

    firm(
        firm_child_storage_ref children,
        firm_join_storage_ref join)
        : child_slots_(children.slots)
        , owned_deed_storage_(children.slots.size())
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , join_failure_slots_(join.failures)
    {
        register_debug();
    }

    firm(
        frame_storage_ref frames,
        firm_child_storage_ref children,
        firm_join_storage_ref join)
        : frames_(frames)
        , child_slots_(children.slots)
        , owned_deed_storage_(children.slots.size())
        , deed_records_(owned_deed_storage_.ref().records)
        , uses_owned_deed_storage_(true)
        , owned_completion_storage_(children.slots.size())
        , completion_slots_(owned_completion_storage_.ref().completions)
        , uses_owned_completion_storage_(true)
        , join_failure_slots_(join.failures)
    {
        register_debug();
    }

    explicit firm(firm_storage_ref storage)
        : frames_(storage.frames)
        , child_slots_(storage.children().slots)
        , deed_records_(storage.deeds().records)
        , completion_slots_(storage.completions().completions)
        , join_failure_slots_(storage.joins().failures)
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
                .children = child_count_,
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
        , owned_child_storage_(std::move(other.owned_child_storage_))
        , child_slots_(
            other.uses_owned_child_storage_
                ? owned_child_storage_.ref().slots
                : other.child_slots_)
        , uses_owned_child_storage_(
            std::exchange(other.uses_owned_child_storage_, false))
        , owned_deed_storage_(std::move(other.owned_deed_storage_))
        , deed_records_(
            other.uses_owned_deed_storage_
                ? owned_deed_storage_.ref().records
                : other.deed_records_)
        , uses_owned_deed_storage_(
            std::exchange(other.uses_owned_deed_storage_, false))
        , owned_completion_storage_(
            std::move(other.owned_completion_storage_))
        , completion_slots_(
            other.uses_owned_completion_storage_
                ? owned_completion_storage_.ref().completions
                : other.completion_slots_)
        , uses_owned_completion_storage_(
            std::exchange(other.uses_owned_completion_storage_, false))
        , owned_join_storage_(std::move(other.owned_join_storage_))
        , join_failure_slots_(
            other.uses_owned_join_storage_
                ? owned_join_storage_.ref().failures
                : other.join_failure_slots_)
        , uses_owned_join_storage_(
            std::exchange(other.uses_owned_join_storage_, false))
        , join_failure_count_(
            std::exchange(other.join_failure_count_, 0))
        , join_failure_high_water_(
            std::exchange(other.join_failure_high_water_, 0))
        , completion_count_(
            std::exchange(other.completion_count_, 0))
        , completion_high_water_(
            std::exchange(other.completion_high_water_, 0))
        , completion_overflow_(
            std::exchange(other.completion_overflow_, false))
        , deed_count_(std::exchange(other.deed_count_, 0))
        , deed_high_water_(
            std::exchange(other.deed_high_water_, 0))
        , child_count_(std::exchange(other.child_count_, 0))
        , child_high_water_(std::exchange(other.child_high_water_, 0))
        , stop_(std::move(other.stop_))
        , debug_id_(std::exchange(other.debug_id_, 0))
        , debug_parent_(std::exchange(other.debug_parent_, 0))
        , stopping_(std::exchange(other.stopping_, false))
    {
        other.child_slots_ = {};
        other.deed_records_ = {};
        other.completion_slots_ = {};
        other.join_failure_slots_ = {};
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

    [[nodiscard]] std::size_t child_capacity() const noexcept
    {
        return child_slots_.size();
    }

    [[nodiscard]] std::size_t child_count() const noexcept
    {
        return child_count_;
    }

    [[nodiscard]] std::size_t child_high_water() const noexcept
    {
        return child_high_water_;
    }

    [[nodiscard]] std::size_t deed_capacity() const noexcept
    {
        return deed_records_.size();
    }

    [[nodiscard]] std::size_t deed_high_water() const noexcept
    {
        return deed_high_water_;
    }

    [[nodiscard]] std::size_t join_failure_capacity() const noexcept
    {
        return join_failure_slots_.size();
    }

    [[nodiscard]] std::size_t join_failure_high_water() const noexcept
    {
        return join_failure_high_water_;
    }

    [[nodiscard]] std::size_t child_completion_capacity() const noexcept
    {
        return completion_slots_.size();
    }

    [[nodiscard]] std::size_t child_completion_high_water() const noexcept
    {
        return completion_high_water_;
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
        if (child_count_ >= child_slots_.size())
            throw runtime_error{"nxtrt firm child storage is full"};
        if (deed_count_ >= deed_records_.size())
            throw runtime_error{"nxtrt firm deed record storage is full"};

        auto handle = child.release();
        if (!handle || handle.done())
            throw runtime_error{"nxtrt firm fork used with empty task"};

        auto result = deed<T>{std::in_place};
        auto * record = static_cast<detail::child_record<T> *>(nullptr);
        auto record_constructed = false;
        try {
            auto & promise = handle.promise();
            // Forked children outlive the call site, so they inherit the
            // current immutable environment snapshot.
            promise.env.copy_entries_from(*current);
            record = &child_slots_[child_count_]
                .template emplace<detail::child_record<T>>(
                handle,
                *this,
                &result.state());
            record_constructed = true;
            promise.observe_completion_of(*record);
            record->firm_record.task =
                active_deck->enqueue(handle, &promise);
            result.state().record.child_task =
                record->firm_record.task;
            deed_records_[deed_count_] = detail::firm_deed_record{
                .child = record->firm_record.task,
            };
        } catch (...) {
            if (record_constructed)
                child_slots_[child_count_].reset();
            else
                handle.destroy();
            throw;
        }

        ++child_count_;
        child_high_water_ = std::max(child_high_water_, child_count_);
        ++deed_count_;
        deed_high_water_ = std::max(deed_high_water_, deed_count_);
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
        for (auto i = std::size_t{0}; i < child_count_; ++i) {
            if (!child_slots_[i].record->joined())
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

protected:
    virtual void child_finished(
        detail::child_record_base &,
        std::exception_ptr) noexcept
    {}

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
        remember_child_completion(child.firm_record.task, failure);
        child_finished(
            child,
            failure);
    }

    void debug_update() const
    {
        debug::update_firm(
            debug::firm_snapshot{
                .id = debug_id_,
                .parent = debug_parent_,
                .children = child_count_,
                .stopping = stopping_,
            });
    }

    template<typename Fn>
    void for_each_child(Fn && fn) noexcept
    {
        for (auto i = std::size_t{0}; i < child_count_; ++i)
            std::invoke(fn, *child_slots_[i].record);
    }

    void remember_join_failure(std::exception_ptr failure)
    {
        if (join_failure_count_ >= join_failure_slots_.size())
            throw runtime_error{
                "nxtrt firm join failure storage is full"};
        join_failure_slots_[join_failure_count_++] = std::move(failure);
        join_failure_high_water_ =
            std::max(join_failure_high_water_, join_failure_count_);
    }

    void remember_child_completion(
        task_id child,
        std::exception_ptr failure) noexcept
    {
        if (completion_count_ >= completion_slots_.size()) {
            completion_overflow_ = true;
            return;
        }
        completion_slots_[completion_count_++] = detail::child_completion{
            .child = child,
            .failure = std::move(failure),
        };
        completion_high_water_ =
            std::max(completion_high_water_, completion_count_);
    }

    void throw_if_completion_overflow()
    {
        if (completion_overflow_)
            throw runtime_error{
                "nxtrt firm child completion storage is full"};
    }

    void clear_join_failures() noexcept
    {
        for (auto i = std::size_t{0}; i < join_failure_count_; ++i)
            join_failure_slots_[i] = {};
        join_failure_count_ = 0;
    }

    [[noreturn]] void throw_frame_arena_full(std::size_t frame_size) const;

    [[noreturn]] void throw_join_failures()
    {
        if (join_failure_count_ == 0)
            throw logic_error{
                "nxtrt firm throw_join_failures called without failures"};
        if (join_failure_count_ == 1)
            rethrow(join_failure_slots_[0]);

        auto exceptions = std::vector<std::exception_ptr>{};
        exceptions.reserve(join_failure_count_);
        for (auto i = std::size_t{0}; i < join_failure_count_; ++i)
            exceptions.push_back(join_failure_slots_[i]);
        throw exception_group{"firm tasks failed", std::move(exceptions)};
    }

    firm_frame_arena frames_;
    owned_firm_child_storage owned_child_storage_;
    std::span<detail::firm_child_slot> child_slots_;
    bool uses_owned_child_storage_ = false;
    owned_firm_deed_storage owned_deed_storage_;
    std::span<detail::firm_deed_record> deed_records_;
    bool uses_owned_deed_storage_ = false;
    owned_firm_completion_storage owned_completion_storage_;
    std::span<detail::child_completion> completion_slots_;
    bool uses_owned_completion_storage_ = false;
    owned_firm_join_storage owned_join_storage_;
    std::span<std::exception_ptr> join_failure_slots_;
    bool uses_owned_join_storage_ = false;
    std::size_t join_failure_count_ = 0;
    std::size_t join_failure_high_water_ = 0;
    std::size_t completion_count_ = 0;
    std::size_t completion_high_water_ = 0;
    bool completion_overflow_ = false;
    std::size_t deed_count_ = 0;
    std::size_t deed_high_water_ = 0;
    std::size_t child_count_ = 0;
    std::size_t child_high_water_ = 0;
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
    struct join_failure_guard
    {
        firm * owner = nullptr;

        ~join_failure_guard() noexcept
        {
            if (owner != nullptr)
                owner->clear_join_failures();
        }
    };

    auto failure_guard = join_failure_guard{this};
    for (auto i = std::size_t{0}; i < child_count_; ++i) {
        auto & record = child_slots_[i].record;
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
            remember_join_failure(std::move(collect_failure));
    }

    throw_if_completion_overflow();

    if (join_failure_count_ != 0)
        throw_join_failures();
}

} // namespace nxtrt
