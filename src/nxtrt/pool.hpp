#pragma once

#include "nxtrt/farm.hpp"
#include "nxtrt/idea.hpp"
#include "nxtrt/task/compose.hpp"

namespace nxtrt {

template<idea Idea>
using pool_result_t = std::conditional_t<
    std::is_void_v<idea_result_t<Idea>>,
    std::monostate,
    std::remove_cv_t<idea_result_t<Idea>>>;

template<idea Idea>
class pool;

/// Stable, reusable residence for one admitted idea. The recipe stays alive
/// through execution and output consumption; a task may borrow its recipe.
/// These objects and their farm must outlive the pool.
template<idea Idea>
class pool_slot : private detail::completion_observer
{
    friend class pool<Idea>;

public:
    pool_slot() = default;
    pool_slot(const pool_slot &) = delete;
    pool_slot & operator=(const pool_slot &) = delete;

private:
    void task_completed() noexcept override;

    pool<Idea> * owner_ = nullptr;
    pool_slot * next_ = nullptr;
    std::optional<Idea> idea_;
    task<idea_result_t<Idea>> task_;
    std::optional<pool_result_t<Idea>> ready_;
    bool started_ = false;
};

/// Concurrent evaluation of a feed of ideas, in completion-publication order.
///
/// Exclusively borrows a fully free farm and at least one output cell per
/// slot. Free + awaiting input + running + completed/unconsumed = capacity.
/// Input recipes are invoked once, only after admission. Ready hopes require
/// no task or scheduler turn. Pending jobs are owned directly, not forked into
/// a firm.
///
/// One consumer, on one deck. Values are consumed according to feed rules:
/// transfer out of this source returns credit, not eventual delivery through
/// every downstream layer. Consumption never starts work or overwrites a view.
///
/// Drain to EOF, or co_await close() before destruction. For early downstream
/// failure use finally(consume(pool), [&pool] { return pool.close(); }), with
/// the pool and its borrowed land outside consume's frame. close cancels and
/// drains input/jobs and discards outstanding outcomes, even if itself stopped.
template<idea Idea>
class pool final
    : public feed<pool_result_t<Idea>>
    , private detail::completion_observer
{
public:
    using result_type = pool_result_t<Idea>;
    using slot_type = pool_slot<Idea>;
    using source_type = feed<result_type>;

    pool(
        feed<Idea> & input,
        farm<slot_type> & slots,
        value_storage_ref<result_type> output)
        : source_type(
              value_storage_ref<result_type>{output.data, slots.capacity()})
        , input_(input)
        , slots_(slots)
    {
        if (slots.capacity() == 0 || output.size < slots.capacity())
            throw runtime_error{
                "nxtrt pool needs nonempty slots and one output cell per slot"};
        this->observe_consumption(
            this,
            [](void * owner, std::size_t n) noexcept {
                static_cast<pool *>(owner)->consumed(n);
            });
    }

    pool(const pool &) = delete;
    pool & operator=(const pool &) = delete;
    pool(pool &&) = delete;
    pool & operator=(pool &&) = delete;

    ~pool()
    {
        if (busy_ || running_ != 0
            || (input_started_ && !input_task_.done())) {
            std::fputs("nxtrt: pool destroyed before close/drain\n", stderr);
            std::abort();
        }
        discard_settled();
    }

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return slots_.capacity();
    }

    /// Includes the reserved input slot and buffered, unconsumed outcomes.
    [[nodiscard]] std::size_t occupied() const noexcept
    {
        return occupied_;
    }

    void stop() noexcept
    {
        stopping_ = true;
        input_task_.request_stop();
        for (auto i = std::size_t{0}; i < capacity(); ++i) {
            auto * slot = slots_.at(i);
            if (slot->owner_ == this)
                slot->task_.request_stop();
        }
        signal();
    }

    /// Idempotent asynchronous teardown. Does not need a result consumer.
    [[nodiscard]] task<void> close()
    {
        bind_deck();
        auto active = operation_guard{*this};
        stop();
        while (!discard_settled())
            co_await changed_awaiter{*this};
    }

private:
    friend class pool_slot<Idea>;

    struct operation_guard
    {
        pool & owner;

        explicit operation_guard(pool & owner)
            : owner(owner)
        {
            if (owner.busy_)
                throw runtime_error{"nxtrt pool has overlapping operations"};
            owner.busy_ = true;
        }

        ~operation_guard()
        {
            owner.busy_ = false;
        }
    };

    struct queue
    {
        slot_type * first = nullptr;
        slot_type * last = nullptr;

        void push(slot_type & slot) noexcept
        {
            slot.next_ = nullptr;
            if (last != nullptr)
                last->next_ = &slot;
            else
                first = &slot;
            last = &slot;
        }

        slot_type & pop() noexcept
        {
            auto & slot = *first;
            first = slot.next_;
            if (first == nullptr)
                last = nullptr;
            slot.next_ = nullptr;
            return slot;
        }
    };

    struct changed_awaiter
    {
        pool & owner;

        bool await_ready() noexcept
        {
            return std::exchange(owner.changed_, false);
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept
        {
            owner.waiter_ = need{handle, current_env()->current_promise};
        }

        void await_resume() noexcept
        {
            owner.changed_ = false;
        }
    };

    void bind_deck()
    {
        auto * current = current_deck();
        if (current == nullptr)
            throw runtime_error{"nxtrt pending pool used without a deck"};
        if (deck_ != nullptr && deck_ != current)
            throw runtime_error{"nxtrt pool used on a different deck"};
        deck_ = current;
    }

    void signal() noexcept
    {
        changed_ = true;
        if (waiter_.handle) {
            auto waiter = std::exchange(waiter_, {});
            waiter.resume(*deck_);
        }
    }

    // Input-read completion uses the same waiter as job completion. An input
    // that is temporarily empty must not block delivery of completed jobs.
    void task_completed() noexcept override
    {
        signal();
    }

    void completed(slot_type & slot) noexcept
    {
        --running_;
        completed_.push(slot);
        signal();
    }

    slot_type * reserve()
    {
        auto * slot = slots_.try_alloc();
        if (slot != nullptr) {
            assert(slot->owner_ == nullptr);
            slot->owner_ = this;
            ++occupied_;
        }
        return slot;
    }

    void release(slot_type & slot) noexcept
    {
        // Frame first, then recipe: coroutine members can borrow their idea.
        slot.task_ = {};
        slot.ready_.reset();
        slot.idea_.reset();
        slot.started_ = false;
        slot.owner_ = nullptr;
        slot.next_ = nullptr;
        --occupied_;
        slots_.release(&slot);
    }

    void consumed(std::size_t n) noexcept
    {
        while (n-- != 0)
            release(buffered_.pop());
    }

    void accept_input(std::optional<Idea> item)
    {
        if (stopping_)
            return;
        auto * slot = std::exchange(input_slot_, nullptr);
        if (!item) {
            input_ended_ = true;
            release(*slot);
            return;
        }
        // Keep ownership visible to teardown if moving/invoking throws.
        slot->idea_.emplace(std::move(*item));
        if (stopping_)
            return;
        if constexpr (is_task_v<std::invoke_result_t<Idea &>>) {
            slot->task_ = std::invoke(*slot->idea_);
        } else {
            auto result = std::invoke(*slot->idea_);
            if (result.is_ready()) {
                if constexpr (std::is_void_v<idea_result_t<Idea>>) {
                    result.take_ready();
                    slot->ready_.emplace();
                } else {
                    slot->ready_.emplace(result.take_ready());
                }
                completed_.push(*slot);
                return;
            }
            slot->task_ = std::move(result).take_pending();
        }
        if (!slot->task_.handle())
            throw runtime_error{"nxtrt pool idea returned an empty task"};
        if (slot->task_.done())
            completed_.push(*slot);
        else
            prepared_ = slot;
    }

    void publish_completed()
    {
        while (completed_.first != nullptr) {
            auto & slot = *completed_.first;
            if (slot.ready_) {
                this->emplace(std::move(*slot.ready_));
            } else if constexpr (std::is_void_v<idea_result_t<Idea>>) {
                std::move(slot.task_).result();
                this->emplace(std::monostate{});
            } else {
                this->emplace(std::move(slot.task_).result());
            }
            // No throwing work between constructing the output and recording
            // its credit. On a throwing move, the slot stays in completed_.
            buffered_.push(completed_.pop());
            slot.task_ = {};
            slot.ready_.reset();
        }
    }

    void pump(bool schedule)
    {
        while (!stopping_) {
            publish_completed();
            if (stopping_)
                return;
            if (prepared_ != nullptr) {
                if (!schedule)
                    return;
                auto & slot = *prepared_;
                slot.task_.handle().promise().observe_completion_of(slot);
                deck_->start(slot.task_);
                slot.started_ = true;
                ++running_;
                prepared_ = nullptr;
            }
            if (input_task_.handle()) {
                if (!input_started_) {
                    if (!schedule)
                        return;
                    input_task_.handle().promise().observe_completion_of(*this);
                    deck_->start(input_task_);
                    input_started_ = true;
                }
                if (!input_task_.done())
                    return;
                auto item = std::move(input_task_).result();
                input_task_ = {};
                input_started_ = false;
                accept_input(std::move(item));
                continue;
            }
            if (input_ended_ || (input_slot_ = reserve()) == nullptr)
                return;
            auto input = input_.take();
            if (stopping_)
                return;
            if (input.is_ready())
                accept_input(input.take_ready());
            else
                input_task_ = std::move(input).take_pending();
        }
    }

    // Teardown never waits on downstream space. Discard all settled state and
    // retain only genuinely running frames (including a pending upstream read).
    bool discard_settled() noexcept
    {
        this->consume_buffered_for_derived(this->buffered_size());
        completed_ = {};
        prepared_ = nullptr;
        if (!input_started_ || input_task_.done()) {
            input_task_ = {};
            input_started_ = false;
            input_slot_ = nullptr;
        }
        for (auto i = std::size_t{0}; i < capacity(); ++i) {
            auto & slot = *slots_.at(i);
            if (slot.owner_ != this || &slot == input_slot_)
                continue;
            if (!slot.started_ || slot.task_.done())
                release(slot);
        }
        return occupied_ == 0;
    }

    hope<fare_t> stream_more(sink<result_type> &, std::size_t limit) override
    {
        auto active = operation_guard{*this};
        if (limit == 0)
            return hope<fare_t>::ready(0);
        auto const before = this->buffered_size();
        try {
            if (stopping_)
                throw operation_cancelled{};
            if (deck_ != nullptr && current_deck() != deck_)
                throw runtime_error{"nxtrt pool used on a different deck"};
            // Pending work is scheduled only inside read_slow, after its
            // controller has a deck entry. Deck exhaustion can then be drained.
            pump(false);
            if (stopping_)
                throw operation_cancelled{};
            if (this->buffered_size() > before)
                return hope<fare_t>::ready(0);
            if (input_ended_ && occupied_ == this->buffered_size())
                return hope<fare_t>::ready(eof);
        } catch (...) {
            failure_ = std::current_exception();
            stop();
            if (discard_settled())
                rethrow(std::exchange(failure_, {}));
        }
        return read_slow(before);
    }

    task<fare_t> read_slow(std::size_t before)
    {
        bind_deck();
        auto active = operation_guard{*this};
        auto stop_pool = [this] { stop(); };
        auto on_stop = std::stop_callback{
            current_task_stop_token(), stop_pool};
        while (true) {
            if (!stopping_) {
                try {
                    pump(true);
                    if (!stopping_ && this->buffered_size() > before)
                        co_return 0;
                    if (!stopping_ && input_ended_
                        && occupied_ == this->buffered_size())
                        co_return eof;
                } catch (...) {
                    failure_ = std::current_exception();
                    stop();
                }
            }
            if (stopping_ && discard_settled()) {
                if (failure_)
                    rethrow(std::exchange(failure_, {}));
                throw operation_cancelled{};
            }
            co_await changed_awaiter{*this};
        }
    }

    feed<Idea> & input_;
    farm<slot_type> & slots_;
    deck * deck_ = nullptr;
    task<std::optional<Idea>> input_task_;
    slot_type * input_slot_ = nullptr;
    slot_type * prepared_ = nullptr;
    queue completed_;
    queue buffered_;
    need waiter_;
    std::exception_ptr failure_;
    std::size_t occupied_ = 0;
    std::size_t running_ = 0;
    bool input_started_ = false;
    bool input_ended_ = false;
    bool changed_ = false;
    bool stopping_ = false;
    bool busy_ = false;
};

template<idea Idea>
void pool_slot<Idea>::task_completed() noexcept
{
    owner_->completed(*this);
}

} // namespace nxtrt
