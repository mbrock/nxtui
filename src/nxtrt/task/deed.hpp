#pragma once

// Child records, result storage, and ordinary or catching deeds.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/task.hpp"

namespace nxtrt {

template<typename T>
class deed_result_storage
{
public:
    using stored_type = std::remove_cv_t<T>;

    deed_result_storage() = default;
    deed_result_storage(const deed_result_storage &) = delete;
    deed_result_storage & operator=(const deed_result_storage &) = delete;
    deed_result_storage(deed_result_storage &&) = delete;
    deed_result_storage & operator=(deed_result_storage &&) = delete;

    [[nodiscard]] bool ready() const noexcept
    {
        return value_.has_value();
    }

    template<typename Value>
        requires std::constructible_from<stored_type, Value &&>
    void set_value(Value && value)
    {
        value_.emplace(std::forward<Value>(value));
    }

    [[nodiscard]] T take_result()
    {
        if (!value_)
            throw runtime_error{"nxtrt task result was never set"};
        auto result = std::move(*value_);
        value_.reset();
        return result;
    }

private:
    std::optional<stored_type> value_;
};

template<typename T>
class deed_result_storage_pool_ref
{
public:
    using storage_type = deed_result_storage<std::remove_cv_t<T>>;

    deed_result_storage_pool_ref() = default;

    explicit deed_result_storage_pool_ref(
        std::span<storage_type> cells,
        std::size_t & used,
        std::size_t & high_water)
        : cells_(cells)
        , used_(&used)
        , high_water_(&high_water)
    {}

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return cells_.size();
    }

    [[nodiscard]] std::size_t used() const noexcept
    {
        return used_ == nullptr ? std::size_t{} : *used_;
    }

    [[nodiscard]] std::size_t high_water() const noexcept
    {
        return high_water_ == nullptr ? std::size_t{} : *high_water_;
    }

    [[nodiscard]] storage_type & borrow()
    {
        if (used_ == nullptr || high_water_ == nullptr)
            throw runtime_error{
                "nxtrt deed result storage pool is empty"};
        if (*used_ >= cells_.size())
            throw runtime_error{
                "nxtrt deed result storage pool is full"};
        auto & cell = cells_[(*used_)++];
        *high_water_ = std::max(*high_water_, *used_);
        return cell;
    }

private:
    std::span<storage_type> cells_;
    std::size_t * used_ = nullptr;
    std::size_t * high_water_ = nullptr;
};

template<typename T, std::size_t N>
class static_deed_result_storage_pool
{
public:
    using storage_type = deed_result_storage<std::remove_cv_t<T>>;

    [[nodiscard]] deed_result_storage_pool_ref<T> ref() noexcept
    {
        return deed_result_storage_pool_ref<T>{
            std::span<storage_type>{storage_.data(), N},
            used_,
            high_water_};
    }

    [[nodiscard]] operator deed_result_storage_pool_ref<T>() noexcept
    {
        return ref();
    }

private:
    std::array<storage_type, N == 0 ? 1 : N> storage_{};
    std::size_t used_ = 0;
    std::size_t high_water_ = 0;
};

namespace detail {

class started_handle_awaiter
{
public:
    template<typename Promise>
    explicit started_handle_awaiter(
        std::coroutine_handle<Promise> handle) noexcept
        : handle_(handle)
        , promise_(handle ? &handle.promise() : nullptr)
    {}

    [[nodiscard]] bool await_ready() const noexcept
    {
        return !handle_ || handle_.done();
    }

    void await_suspend(std::coroutine_handle<> awaiting) const
    {
        auto * current = detail::current_env;
        auto * awaiting_promise =
            current == nullptr ? nullptr : current->current_promise;
        if (awaiting_promise == nullptr || promise_ == nullptr)
            throw runtime_error{
                "nxtrt firm join used without a running task"};

        promise_->set_continuation(awaiting, awaiting_promise);
    }

    void await_resume() const noexcept {}

private:
    std::coroutine_handle<> handle_;
    promise_base * promise_ = nullptr;
};

struct firm_child_record_header
{
    firm * owner = nullptr;
    task_id task;
    bool completion_reported = false;
    bool result_observed = false;
};

struct child_record_base
{
    child_record_base() = default;
    child_record_base(const child_record_base &) = delete;
    child_record_base & operator=(const child_record_base &) = delete;
    child_record_base(child_record_base &&) = delete;
    child_record_base & operator=(child_record_base &&) = delete;
    virtual ~child_record_base() = default;

    [[nodiscard]] virtual bool done() const noexcept = 0;
    [[nodiscard]] virtual bool joined() const noexcept = 0;
    [[nodiscard]] virtual std::exception_ptr completion_failure()
        noexcept = 0;
    [[nodiscard]] virtual std::exception_ptr failure() = 0;
    [[nodiscard]] virtual bool result_contained() const noexcept = 0;
    [[nodiscard]] virtual bool result_observed() const noexcept = 0;
    [[nodiscard]] virtual bool result_exported() const noexcept = 0;
    [[nodiscard]] virtual task<void> join() = 0;
    virtual void evacuate_result_if_done() = 0;
    virtual void drop_result_state(
        deed_result_state_base * result) noexcept = 0;
    virtual void replace_result_state(
        deed_result_state_base * old_result,
        deed_result_state_base * new_result) noexcept = 0;
    virtual void request_stop() noexcept = 0;

    void report_finished_from_promise() noexcept;

    firm_child_record_header firm_record;
};

struct deed_record_header
{
    child_record_base * child = nullptr;
    task_id child_task;
    bool contained = false;
    bool observed = false;
    bool result_taken = false;
};

struct deed_result_state_base
{
    deed_result_state_base() = default;
    deed_result_state_base(const deed_result_state_base &) = delete;
    deed_result_state_base & operator=(
        const deed_result_state_base &) = delete;
    deed_result_state_base(deed_result_state_base && other) noexcept
        : record{
              .child = std::exchange(other.record.child, nullptr),
              .child_task = std::exchange(other.record.child_task, {}),
              .contained = std::exchange(other.record.contained, false),
              .observed = std::exchange(other.record.observed, false),
              .result_taken =
                  std::exchange(other.record.result_taken, false),
          }
    {
        if (record.child != nullptr)
            record.child->replace_result_state(&other, this);
    }

    deed_result_state_base & operator=(
        deed_result_state_base && other) noexcept
    {
        if (this == &other)
            return *this;
        detach();
        record.child = std::exchange(other.record.child, nullptr);
        record.child_task = std::exchange(other.record.child_task, {});
        record.contained =
            std::exchange(other.record.contained, false);
        record.observed = std::exchange(other.record.observed, false);
        record.result_taken =
            std::exchange(other.record.result_taken, false);
        if (record.child != nullptr)
            record.child->replace_result_state(&other, this);
        return *this;
    }

    virtual ~deed_result_state_base()
    {
        detach();
    }

    void detach() noexcept
    {
        if (record.child == nullptr)
            return;
        auto * old_child = record.child;
        record.child = nullptr;
        old_child->drop_result_state(this);
    }

    void ensure_ready_from_child()
    {
        if (record.child != nullptr && record.child->done())
            record.child->evacuate_result_if_done();
    }

    deed_record_header record;
};

template<typename T>
struct deed_result_slot
{
    using stored_type = std::remove_cv_t<T>;
    using storage_type =
        std::variant<
            std::monostate,
            stored_type,
            stored_type *,
            deed_result_storage<stored_type> *,
            std::exception_ptr>;

    [[nodiscard]] bool ready() const noexcept
    {
        if (std::holds_alternative<stored_type *>(storage))
            return target_ready;
        if (auto * target = target_cell())
            return target->ready();
        return !std::holds_alternative<std::monostate>(storage);
    }

    template<typename Value>
    void set_value(Value && value)
    {
        if (auto * target = target_storage()) {
            if constexpr (std::assignable_from<stored_type &, Value>) {
                *target = std::forward<Value>(value);
                target_ready = true;
                return;
            } else {
                throw runtime_error{
                    "nxtrt deed result target is not assignable"};
            }
        }
        if (auto * target = target_cell()) {
            if constexpr (std::constructible_from<
                              stored_type,
                              Value &&>) {
                target->set_value(std::forward<Value>(value));
                return;
            } else {
                throw runtime_error{
                    "nxtrt deed result target is not constructible"};
            }
        }
        storage.template emplace<stored_type>(
            std::forward<Value>(value));
    }

    void set_exception(std::exception_ptr failure)
    {
        storage.template emplace<std::exception_ptr>(
            std::move(failure));
        target_ready = false;
    }

    [[nodiscard]] std::exception_ptr failure() const
    {
        if (std::holds_alternative<std::exception_ptr>(storage))
            return std::get<std::exception_ptr>(storage);
        return {};
    }

    [[nodiscard]] T take_result()
    {
        if (std::holds_alternative<std::exception_ptr>(storage))
            rethrow(std::get<std::exception_ptr>(storage));
        if (auto * target = target_storage()) {
            if (!target_ready)
                throw runtime_error{"nxtrt task result was never set"};
            return std::move(*target);
        }
        if (auto * target = target_cell())
            return target->take_result();
        if (!std::holds_alternative<stored_type>(storage))
            throw runtime_error{"nxtrt task result was never set"};
        return std::move(std::get<stored_type>(storage));
    }

    void store_in(stored_type & target)
    {
        if (std::holds_alternative<std::exception_ptr>(storage))
            throw runtime_error{
                "nxtrt deed result target set after failure"};
        if (auto * old_target = target_storage()) {
            if (target_ready)
                throw runtime_error{
                    "nxtrt deed result target set after result ready"};
            if (old_target == &target)
                return;
        } else if (auto * old_target = target_cell()) {
            if (old_target->ready())
                throw runtime_error{
                    "nxtrt deed result target set after result ready"};
        } else if (std::holds_alternative<stored_type>(storage)) {
            target = std::move(std::get<stored_type>(storage));
            target_ready = true;
        }
        storage.template emplace<stored_type *>(&target);
    }

    void store_in(deed_result_storage<stored_type> & target)
    {
        if (std::holds_alternative<std::exception_ptr>(storage))
            throw runtime_error{
                "nxtrt deed result target set after failure"};
        if (target.ready())
            throw runtime_error{
                "nxtrt deed result target set after result ready"};
        if (auto * old_target = target_storage()) {
            if (target_ready)
                throw runtime_error{
                    "nxtrt deed result target set after result ready"};
            static_cast<void>(old_target);
        } else if (auto * old_target = target_cell()) {
            if (old_target->ready())
                throw runtime_error{
                    "nxtrt deed result target set after result ready"};
            if (old_target == &target)
                return;
        } else if (std::holds_alternative<stored_type>(storage)) {
            target.set_value(std::move(std::get<stored_type>(storage)));
        }
        storage.template emplace<deed_result_storage<stored_type> *>(
            &target);
        target_ready = false;
    }

    [[nodiscard]] stored_type * target_storage() noexcept
    {
        if (std::holds_alternative<stored_type *>(storage))
            return std::get<stored_type *>(storage);
        return nullptr;
    }

    [[nodiscard]] const stored_type * target_storage() const noexcept
    {
        if (std::holds_alternative<stored_type *>(storage))
            return std::get<stored_type *>(storage);
        return nullptr;
    }

    [[nodiscard]] deed_result_storage<stored_type> *
    target_cell() noexcept
    {
        if (std::holds_alternative<deed_result_storage<stored_type> *>(
                storage))
            return std::get<deed_result_storage<stored_type> *>(storage);
        return nullptr;
    }

    [[nodiscard]] const deed_result_storage<stored_type> *
    target_cell() const noexcept
    {
        if (std::holds_alternative<deed_result_storage<stored_type> *>(
                storage))
            return std::get<deed_result_storage<stored_type> *>(storage);
        return nullptr;
    }

    storage_type storage;
    bool target_ready = false;
};

template<>
struct deed_result_slot<void>
{
    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }

    void set_value() noexcept
    {
        ready_ = true;
    }

    void set_exception(std::exception_ptr failure)
    {
        failure_ = std::move(failure);
        ready_ = true;
    }

    [[nodiscard]] std::exception_ptr failure() const
    {
        return failure_;
    }

    void take_result()
    {
        if (failure_)
            rethrow(failure_);
    }

    std::exception_ptr failure_;
    bool ready_ = false;
};

template<typename T>
struct deed_result_state final : deed_result_state_base
{
    using stored_type = typename deed_result_slot<T>::stored_type;

    deed_result_state() = default;
    deed_result_state(deed_result_state &&) = default;
    deed_result_state & operator=(deed_result_state &&) = default;

    [[nodiscard]] bool ready() const noexcept
    {
        return slot.ready();
    }

    void ensure_done()
    {
        if (!ready())
            ensure_ready_from_child();
        if (!ready())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    template<typename Value>
    void set_value(Value && value)
    {
        slot.set_value(std::forward<Value>(value));
    }

    void set_exception(std::exception_ptr failure)
    {
        slot.set_exception(std::move(failure));
    }

    [[nodiscard]] std::exception_ptr failure()
    {
        ensure_done();
        return slot.failure();
    }

    [[nodiscard]] std::exception_ptr observe_exception()
    {
        record.observed = true;
        return failure();
    }

    [[nodiscard]] T take_result()
    {
        ensure_done();
        if (record.result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        record.observed = true;
        record.result_taken = true;
        return slot.take_result();
    }

    void store_result_in(stored_type & target)
    {
        if (record.result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        if (!ready())
            ensure_ready_from_child();
        slot.store_in(target);
    }

    void store_result_in(deed_result_storage<stored_type> & target)
    {
        if (record.result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        if (!ready())
            ensure_ready_from_child();
        slot.store_in(target);
    }

    deed_result_slot<T> slot;
};

template<>
struct deed_result_state<void> final : deed_result_state_base
{
    deed_result_state() = default;
    deed_result_state(deed_result_state &&) noexcept = default;
    deed_result_state & operator=(deed_result_state &&) noexcept = default;

    [[nodiscard]] bool ready() const noexcept
    {
        return slot.ready();
    }

    void ensure_done()
    {
        if (!ready())
            ensure_ready_from_child();
        if (!ready())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    void set_value() noexcept
    {
        slot.set_value();
    }

    void set_exception(std::exception_ptr failure)
    {
        slot.set_exception(std::move(failure));
    }

    [[nodiscard]] std::exception_ptr failure()
    {
        ensure_done();
        return slot.failure();
    }

    [[nodiscard]] std::exception_ptr observe_exception()
    {
        record.observed = true;
        return failure();
    }

    void take_result()
    {
        ensure_done();
        if (record.result_taken)
            throw runtime_error{"nxtrt deed result already taken"};
        record.observed = true;
        record.result_taken = true;
        slot.take_result();
    }

    deed_result_slot<void> slot;
};

template<typename T>
struct child_record final : child_record_base
{
    using handle_type = typename task<T>::coroutine_handle;

    child_record(
        handle_type h,
        firm & owner,
        deed_result_state<T> * result)
        : handle(h)
        , result(result)
    {
        this->firm_record.owner = &owner;
        if (this->result != nullptr)
            this->result->record.child = this;
    }

    ~child_record() override
    {
        if (result != nullptr)
            result->record.child = nullptr;
        destroy_frame();
    }

    [[nodiscard]] bool done() const noexcept override
    {
        return evacuated_ || !handle || handle.done();
    }

    [[nodiscard]] bool joined() const noexcept override
    {
        return joined_;
    }

    [[nodiscard]] task<void> join() override
    {
        if (!joined_ && handle && !handle.done())
            co_await started_handle_awaiter{handle};
        joined_ = true;
        evacuate_result();
    }

    [[nodiscard]] std::exception_ptr completion_failure()
        noexcept override
    {
        if (!handle || !handle.done())
            return {};
        try {
            static_cast<void>(handle.promise().result());
        } catch (...) {
            return std::current_exception();
        }
        return {};
    }

    [[nodiscard]] std::exception_ptr failure() override
    {
        evacuate_result();
        if (result != nullptr)
            return result->failure();
        return failure_;
    }

    [[nodiscard]] bool result_contained() const noexcept override
    {
        return result != nullptr && result->record.contained;
    }

    [[nodiscard]] bool result_observed() const noexcept override
    {
        return this->firm_record.result_observed
               || (result != nullptr && result->record.observed);
    }

    [[nodiscard]] bool result_exported() const noexcept override
    {
        return result != nullptr;
    }

    void evacuate_result_if_done() override
    {
        if (done())
            evacuate_result();
    }

    void drop_result_state(deed_result_state_base * state) noexcept override
    {
        if (state == result) {
            this->firm_record.result_observed = state->record.observed;
            result = nullptr;
        }
    }

    void replace_result_state(
        deed_result_state_base * old_state,
        deed_result_state_base * new_state) noexcept override
    {
        if (old_state == result)
            result = static_cast<deed_result_state<T> *>(new_state);
    }

    void request_stop() noexcept override
    {
        if (handle)
            handle.promise().request_stop();
    }

    void ensure_done() const
    {
        if (!done())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    void evacuate_result()
    {
        ensure_done();
        if (evacuated_)
            return;
        try {
            if (result != nullptr)
                result->set_value(std::move(handle.promise()).result());
            else
                static_cast<void>(std::move(handle.promise()).result());
        } catch (...) {
            failure_ = std::current_exception();
            if (result != nullptr)
                result->set_exception(failure_);
        }
        evacuated_ = true;
        // Keep the bookkeeping link until either side dies. The frame is
        // gone, but a returned/moved deed must still retarget this record.
        destroy_frame();
    }

    void destroy_frame() noexcept
    {
        if (!handle)
            return;
        debug::unpark_task(handle.promise().id);
        handle.promise().unregister_from_deck();
        auto dying = handle;
        handle = handle_type{};
        dying.destroy();
    }

    handle_type handle;
    deed_result_state<T> * result = nullptr;
    std::exception_ptr failure_;
    bool joined_ = false;
    bool evacuated_ = false;
};

template<>
struct child_record<void> final : child_record_base
{
    using handle_type = typename task<void>::coroutine_handle;

    child_record(
        handle_type h,
        firm & owner,
        deed_result_state<void> * result)
        : handle(h)
        , result(result)
    {
        this->firm_record.owner = &owner;
        if (this->result != nullptr)
            this->result->record.child = this;
    }

    ~child_record() override
    {
        if (result != nullptr)
            result->record.child = nullptr;
        destroy_frame();
    }

    [[nodiscard]] bool done() const noexcept override
    {
        return evacuated_ || !handle || handle.done();
    }

    [[nodiscard]] bool joined() const noexcept override
    {
        return joined_;
    }

    [[nodiscard]] task<void> join() override
    {
        if (!joined_ && handle && !handle.done())
            co_await started_handle_awaiter{handle};
        joined_ = true;
        evacuate_result();
    }

    [[nodiscard]] std::exception_ptr completion_failure()
        noexcept override
    {
        if (!handle || !handle.done())
            return {};
        try {
            handle.promise().result();
        } catch (...) {
            return std::current_exception();
        }
        return {};
    }

    [[nodiscard]] std::exception_ptr failure() override
    {
        evacuate_result();
        if (result != nullptr)
            return result->failure();
        return failure_;
    }

    [[nodiscard]] bool result_contained() const noexcept override
    {
        return result != nullptr && result->record.contained;
    }

    [[nodiscard]] bool result_observed() const noexcept override
    {
        return this->firm_record.result_observed
               || (result != nullptr && result->record.observed);
    }

    [[nodiscard]] bool result_exported() const noexcept override
    {
        return result != nullptr;
    }

    void evacuate_result_if_done() override
    {
        if (done())
            evacuate_result();
    }

    void drop_result_state(deed_result_state_base * state) noexcept override
    {
        if (state == result) {
            this->firm_record.result_observed = state->record.observed;
            result = nullptr;
        }
    }

    void replace_result_state(
        deed_result_state_base * old_state,
        deed_result_state_base * new_state) noexcept override
    {
        if (old_state == result)
            result = static_cast<deed_result_state<void> *>(new_state);
    }

    void request_stop() noexcept override
    {
        if (handle)
            handle.promise().request_stop();
    }

    void ensure_done() const
    {
        if (!done())
            throw runtime_error{
                "nxtrt deed result read before firm join"};
    }

    void evacuate_result()
    {
        ensure_done();
        if (evacuated_)
            return;
        try {
            handle.promise().result();
            if (result != nullptr)
                result->set_value();
        } catch (...) {
            failure_ = std::current_exception();
            if (result != nullptr)
                result->set_exception(failure_);
        }
        evacuated_ = true;
        // As for valued deeds, moves still retarget the settlement record
        // after the task frame has been evacuated.
        destroy_frame();
    }

    void destroy_frame() noexcept
    {
        if (!handle)
            return;
        debug::unpark_task(handle.promise().id);
        handle.promise().unregister_from_deck();
        auto dying = handle;
        handle = handle_type{};
        dying.destroy();
    }

    handle_type handle;
    deed_result_state<void> * result = nullptr;
    std::exception_ptr failure_;
    bool joined_ = false;
    bool evacuated_ = false;
};

} // namespace detail

template<typename T>
class deed
{
public:
    deed() = default;
    deed(const deed &) = delete;
    deed & operator=(const deed &) = delete;
    deed(deed && other) noexcept(
        std::is_nothrow_move_constructible_v<
            detail::deed_result_state<T>>)
        : state_(std::move(other.state_))
    {
        other.state_.reset();
    }

    deed & operator=(deed && other) noexcept(
        std::is_nothrow_move_assignable_v<
            std::optional<detail::deed_result_state<T>>>)
    {
        if (this == &other)
            return *this;
        state_ = std::move(other.state_);
        other.state_.reset();
        return *this;
    }

    [[nodiscard]] std::exception_ptr exception() const
    {
        return state().observe_exception();
    }

    [[nodiscard]] task_id child_task_id() const
    {
        return state().record.child_task;
    }

    deed & store_result_in(std::remove_cv_t<T> & target)
        requires std::assignable_from<std::remove_cv_t<T> &, T>
    {
        state().store_result_in(target);
        return *this;
    }

    deed & store_result_in(
        deed_result_storage<std::remove_cv_t<T>> & target)
        requires std::constructible_from<std::remove_cv_t<T>, T>
    {
        state().store_result_in(target);
        return *this;
    }

    [[nodiscard]] T get() &&
    {
        return state().take_result();
    }

    [[nodiscard]] catching_deed<T> cope() &&;

private:
    friend class firm;
    friend class catching_deed<T>;

    explicit deed(std::in_place_t)
        : state_(std::in_place)
    {}

    [[nodiscard]] detail::deed_result_state<T> & state() const
    {
        if (!state_)
            throw runtime_error{"nxtrt empty deed handle"};
        return *state_;
    }

    mutable std::optional<detail::deed_result_state<T>> state_;
};

template<>
class deed<void>
{
public:
    deed() = default;
    deed(const deed &) = delete;
    deed & operator=(const deed &) = delete;
    deed(deed && other) noexcept
        : state_(std::move(other.state_))
    {
        other.state_.reset();
    }

    deed & operator=(deed && other) noexcept
    {
        if (this == &other)
            return *this;
        state_ = std::move(other.state_);
        other.state_.reset();
        return *this;
    }

    [[nodiscard]] std::exception_ptr exception() const
    {
        return state().observe_exception();
    }

    [[nodiscard]] task_id child_task_id() const
    {
        return state().record.child_task;
    }

    void get() &&
    {
        state().take_result();
    }

    [[nodiscard]] catching_deed<void> cope() &&;

private:
    friend class firm;
    friend class catching_deed<void>;

    explicit deed(std::in_place_t)
        : state_(std::in_place)
    {}

    [[nodiscard]] detail::deed_result_state<void> & state() const
    {
        if (!state_)
            throw runtime_error{"nxtrt empty deed handle"};
        return *state_;
    }

    mutable std::optional<detail::deed_result_state<void>> state_;
};

template<typename T>
class catching_deed
{
public:
    catching_deed() = default;
    catching_deed(const catching_deed &) = delete;
    catching_deed & operator=(const catching_deed &) = delete;
    catching_deed(catching_deed && other) noexcept(
        std::is_nothrow_move_constructible_v<
            detail::deed_result_state<T>>)
        : state_(std::move(other.state_))
    {
        other.state_.reset();
    }

    catching_deed & operator=(catching_deed && other) noexcept(
        std::is_nothrow_move_assignable_v<
            std::optional<detail::deed_result_state<T>>>)
    {
        if (this == &other)
            return *this;
        state_ = std::move(other.state_);
        other.state_.reset();
        return *this;
    }

    [[nodiscard]] std::expected<T, std::exception_ptr> get() &&
    {
        auto & child = state();
        child.ensure_done();
        try {
            return child.take_result();
        } catch (...) {
            return std::unexpected{std::current_exception()};
        }
    }

    [[nodiscard]] task_id child_task_id() const
    {
        if (!state_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return state_->record.child_task;
    }

private:
    friend class deed<T>;

    explicit catching_deed(detail::deed_result_state<T> && state)
        : state_(std::in_place, std::move(state))
    {}

    [[nodiscard]] detail::deed_result_state<T> & state()
    {
        if (!state_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return *state_;
    }

    std::optional<detail::deed_result_state<T>> state_;
};

template<>
class catching_deed<void>
{
public:
    catching_deed() = default;
    catching_deed(const catching_deed &) = delete;
    catching_deed & operator=(const catching_deed &) = delete;
    catching_deed(catching_deed && other) noexcept
        : state_(std::move(other.state_))
    {
        other.state_.reset();
    }

    catching_deed & operator=(catching_deed && other) noexcept
    {
        if (this == &other)
            return *this;
        state_ = std::move(other.state_);
        other.state_.reset();
        return *this;
    }

    [[nodiscard]] std::expected<void, std::exception_ptr> get() &&
    {
        auto & child = state();
        child.ensure_done();
        try {
            child.take_result();
            return {};
        } catch (...) {
            return std::unexpected{std::current_exception()};
        }
    }

    [[nodiscard]] task_id child_task_id() const
    {
        if (!state_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return state_->record.child_task;
    }

private:
    friend class deed<void>;

    explicit catching_deed(detail::deed_result_state<void> && state)
        : state_(std::in_place, std::move(state))
    {}

    [[nodiscard]] detail::deed_result_state<void> & state()
    {
        if (!state_)
            throw runtime_error{"nxtrt empty catching_deed handle"};
        return *state_;
    }

    std::optional<detail::deed_result_state<void>> state_;
};

template<typename T>
inline catching_deed<T> deed<T>::cope() &&
{
    if (!state_)
        throw runtime_error{"nxtrt empty deed handle"};
    state_->record.contained = true;
    auto result = catching_deed<T>{std::move(*state_)};
    state_.reset();
    return result;
}

inline catching_deed<void> deed<void>::cope() &&
{
    if (!state_)
        throw runtime_error{"nxtrt empty deed handle"};
    state_->record.contained = true;
    auto result = catching_deed<void>{std::move(*state_)};
    state_.reset();
    return result;
}

} // namespace nxtrt
