#pragma once

// Experimental coroutine task for exploring a richer nxt async environment.
//
// This sits in the same conceptual neighborhood as the ongoing experiments
// around layouts-as-actors, structured concurrency, and nested habitats: a
// layout/process/task is not only "some code that eventually returns", but a
// little place where work runs, local capabilities are ambient, children belong
// to parents, and cancellation/trace/context can follow the shape of the UI.
// The names here are deliberately provisional while that model settles.
//
// `nxt::yardtask<T>` is a custom coroutine return object, not just an alias to
// `coro::task<T>`. Its promise owns a `yard_frame`: a tiny ambient execution
// record containing:
//
// - a Python contextvars-like `yard_context`
// - a parent pointer and child list for structured-concurrency relations
// - a stop source used to propagate cancellation down the frame tree
//
// The key experiment lives in the promise's `await_transform`. Every awaitable
// seen inside a `yardtask` is wrapped in an ambient awaiter that restores the
// current `yard_frame` before `await_ready`, `await_suspend`, and
// `await_resume`. That means ordinary libcoro awaitables such as scheduler
// sleeps/polls can still be awaited directly, while code resumed after the
// await sees the same ambient yard context.
//
// Child `yardtask`s inherit the current frame at coroutine construction time.
// Awaiting a child gives it a parent relation and a snapshot of the parent's
// context. `yard_spawn(scheduler, task)` starts a child on a libcoro scheduler
// and returns a join handle, giving us a place to model "spawned but still
// structured" work.
//
// This is intentionally a scaffold, not a settled runtime contract. Open design
// questions include the exact context mutation semantics, how strongly to own
// child lifetimes, how to surface errors from spawned children, and whether the
// ambient frame should become thread-local, scheduler-local, or explicit in a
// future multi-threaded runtime.
//
#include "nxtio/async-core.hpp"

#include <any>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nxt {

template<typename T = void>
class yardtask;

class yard_context
{
public:
    template<typename T>
    void set(std::string key, T value)
    {
        values_[std::move(key)] = std::move(value);
    }

    template<typename T>
    [[nodiscard]] T * get_if(std::string_view key) noexcept
    {
        auto it = values_.find(std::string{key});
        if (it == values_.end())
            return nullptr;
        return std::any_cast<T>(&it->second);
    }

    template<typename T>
    [[nodiscard]] const T * get_if(std::string_view key) const noexcept
    {
        auto it = values_.find(std::string{key});
        if (it == values_.end())
            return nullptr;
        return std::any_cast<T>(&it->second);
    }

    [[nodiscard]] bool contains(std::string_view key) const
    {
        return values_.contains(std::string{key});
    }

private:
    std::unordered_map<std::string, std::any> values_;
};

struct yard_frame
{
    using callback_type = std::stop_callback<std::function<void()>>;

    std::uint64_t id = next_id.fetch_add(1, std::memory_order::relaxed);
    std::string name;
    yard_context context;
    std::stop_source stop_source;
    std::weak_ptr<yard_frame> parent;
    std::vector<std::weak_ptr<yard_frame>> children;
    std::optional<callback_type> parent_stop_callback;

    ~yard_frame()
    {
        request_stop();
    }

    void request_stop() noexcept
    {
        stop_source.request_stop();
        for (auto & weak_child : children) {
            if (auto child = weak_child.lock())
                child->request_stop();
        }
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stop_source.stop_requested();
    }

    [[nodiscard]] std::stop_token stop_token() const noexcept
    {
        return stop_source.get_token();
    }

private:
    inline static std::atomic_uint64_t next_id{1};
};

namespace detail {

inline thread_local std::shared_ptr<yard_frame> current_yard_frame;

class yard_frame_guard
{
public:
    explicit yard_frame_guard(std::shared_ptr<yard_frame> frame)
        : previous_(std::move(current_yard_frame))
    {
        current_yard_frame = std::move(frame);
    }

    yard_frame_guard(const yard_frame_guard &) = delete;
    yard_frame_guard & operator=(const yard_frame_guard &) = delete;

    ~yard_frame_guard()
    {
        current_yard_frame = std::move(previous_);
    }

private:
    std::shared_ptr<yard_frame> previous_;
};

inline void set_current_yard_frame(std::shared_ptr<yard_frame> frame)
{
    current_yard_frame = std::move(frame);
}

inline void inherit_yard_frame(const std::shared_ptr<yard_frame> & child)
{
    auto parent = current_yard_frame;
    if (!parent)
        return;

    child->parent = parent;
    child->context = parent->context;
    child->parent_stop_callback.emplace(
        parent->stop_token(),
        [weak_child = std::weak_ptr<yard_frame>{child}] {
            if (auto locked = weak_child.lock())
                locked->request_stop();
        });
    parent->children.push_back(child);
}

template<typename Awaitable>
decltype(auto) get_awaiter(Awaitable && awaitable)
{
    if constexpr (requires {
                      std::forward<Awaitable>(awaitable).operator co_await();
                  }) {
        return std::forward<Awaitable>(awaitable).operator co_await();
    } else if constexpr (requires {
                             operator co_await(
                                 std::forward<Awaitable>(awaitable));
                         }) {
        return operator co_await(std::forward<Awaitable>(awaitable));
    } else {
        return std::forward<Awaitable>(awaitable);
    }
}

template<typename Awaitable>
using ambient_stored_awaitable_t = std::conditional_t<
    std::is_lvalue_reference_v<Awaitable>,
    Awaitable,
    std::remove_cvref_t<Awaitable>>;

template<typename Awaitable>
decltype(auto)
ambient_awaitable_arg(ambient_stored_awaitable_t<Awaitable> & awaitable)
{
    if constexpr (std::is_lvalue_reference_v<Awaitable>)
        return awaitable;
    else
        return std::move(awaitable);
}

template<typename Awaitable>
class ambient_awaitable
{
    using stored_awaitable = ambient_stored_awaitable_t<Awaitable>;
    using awaiter_type = decltype(get_awaiter(
        ambient_awaitable_arg<Awaitable>(
            std::declval<stored_awaitable &>())));

public:
    ambient_awaitable(
        std::shared_ptr<yard_frame> frame,
        Awaitable awaitable)
        : frame_(std::move(frame))
        , awaitable_(std::forward<Awaitable>(awaitable))
        , awaiter_(get_awaiter(
              ambient_awaitable_arg<Awaitable>(awaitable_)))
    {
    }

    [[nodiscard]] bool await_ready()
        noexcept(noexcept(std::declval<awaiter_type &>().await_ready()))
    {
        set_current_yard_frame(frame_);
        return awaiter_.await_ready();
    }

    decltype(auto) await_suspend(std::coroutine_handle<> awaiting)
        noexcept(noexcept(std::declval<awaiter_type &>().await_suspend(
            awaiting)))
    {
        set_current_yard_frame(frame_);
        if constexpr (std::is_void_v<decltype(awaiter_.await_suspend(
                          awaiting))>) {
            awaiter_.await_suspend(awaiting);
        } else {
            return awaiter_.await_suspend(awaiting);
        }
    }

    decltype(auto) await_resume()
        noexcept(noexcept(std::declval<awaiter_type &>().await_resume()))
    {
        set_current_yard_frame(frame_);
        return awaiter_.await_resume();
    }

private:
    std::shared_ptr<yard_frame> frame_;
    stored_awaitable awaitable_;
    awaiter_type awaiter_;
};

struct yard_promise_base
{
    struct final_awaitable
    {
        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        template<typename Promise>
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<Promise> coroutine) noexcept
        {
            auto & promise = coroutine.promise();
            promise.frame_->request_stop();
            set_current_yard_frame(promise.continuation_frame_.lock());
            if (promise.continuation_)
                return promise.continuation_;
            return std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    yard_promise_base()
        : frame_(std::make_shared<yard_frame>())
    {
        inherit_yard_frame(frame_);
    }

    [[nodiscard]] auto initial_suspend() noexcept
    {
        return std::suspend_always{};
    }

    [[nodiscard]] auto final_suspend() noexcept
    {
        return final_awaitable{};
    }

    void unhandled_exception() noexcept
    {
        exception_ = std::current_exception();
    }

    void set_continuation(
        std::coroutine_handle<> continuation,
        std::shared_ptr<yard_frame> continuation_frame) noexcept
    {
        continuation_ = continuation;
        continuation_frame_ = std::move(continuation_frame);
    }

    template<typename Awaitable>
    [[nodiscard]] auto await_transform(Awaitable && awaitable)
    {
        return ambient_awaitable<Awaitable>{
            frame_, std::forward<Awaitable>(awaitable)};
    }

    std::shared_ptr<yard_frame> frame_;
    std::coroutine_handle<> continuation_;
    std::weak_ptr<yard_frame> continuation_frame_;
    std::exception_ptr exception_;
};

template<typename T>
struct yard_promise final : yard_promise_base
{
    using task_type = yardtask<T>;
    using stored_type = std::remove_cv_t<T>;
    using storage_type =
        std::variant<std::monostate, stored_type, std::exception_ptr>;

    [[nodiscard]] task_type get_return_object() noexcept;

    template<typename Value>
        requires std::is_constructible_v<stored_type, Value &&>
    void return_value(Value && value)
    {
        storage_.template emplace<stored_type>(
            std::forward<Value>(value));
    }

    void unhandled_exception() noexcept
    {
        storage_.template emplace<std::exception_ptr>(
            std::current_exception());
    }

    T & result() &
    {
        if (std::holds_alternative<stored_type>(storage_))
            return std::get<stored_type>(storage_);
        if (std::holds_alternative<std::exception_ptr>(storage_))
            std::rethrow_exception(
                std::get<std::exception_ptr>(storage_));
        throw std::runtime_error{"yardtask result was never set"};
    }

    const T & result() const &
    {
        if (std::holds_alternative<stored_type>(storage_))
            return std::get<stored_type>(storage_);
        if (std::holds_alternative<std::exception_ptr>(storage_))
            std::rethrow_exception(
                std::get<std::exception_ptr>(storage_));
        throw std::runtime_error{"yardtask result was never set"};
    }

    T && result() &&
    {
        if (std::holds_alternative<stored_type>(storage_))
            return std::move(std::get<stored_type>(storage_));
        if (std::holds_alternative<std::exception_ptr>(storage_))
            std::rethrow_exception(
                std::get<std::exception_ptr>(storage_));
        throw std::runtime_error{"yardtask result was never set"};
    }

private:
    storage_type storage_;
};

template<>
struct yard_promise<void> final : yard_promise_base
{
    using task_type = yardtask<void>;

    [[nodiscard]] task_type get_return_object() noexcept;

    void return_void() noexcept {}

    void result()
    {
        if (exception_)
            std::rethrow_exception(exception_);
    }
};

} // namespace detail

template<typename T>
class [[nodiscard]] yardtask
{
public:
    using promise_type = detail::yard_promise<T>;
    using coroutine_handle = std::coroutine_handle<promise_type>;

    class awaiter
    {
    public:
        explicit awaiter(coroutine_handle coroutine) noexcept
            : coroutine_(coroutine)
        {
        }

        [[nodiscard]] bool await_ready() const noexcept
        {
            return !coroutine_ || coroutine_.done();
        }

        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<> awaiting) noexcept
        {
            auto continuation_frame = detail::current_yard_frame;
            coroutine_.promise().set_continuation(
                awaiting, std::move(continuation_frame));
            detail::set_current_yard_frame(coroutine_.promise().frame_);
            return coroutine_;
        }

    protected:
        coroutine_handle coroutine_;
    };

    class lvalue_awaiter : public awaiter
    {
    public:
        using awaiter::awaiter;

        decltype(auto) await_resume()
        {
            return this->coroutine_.promise().result();
        }
    };

    class rvalue_awaiter : public awaiter
    {
    public:
        using awaiter::awaiter;

        decltype(auto) await_resume()
        {
            return std::move(this->coroutine_.promise()).result();
        }
    };

    yardtask() noexcept = default;

    explicit yardtask(coroutine_handle coroutine) noexcept
        : coroutine_(coroutine)
    {
    }

    yardtask(const yardtask &) = delete;
    yardtask & operator=(const yardtask &) = delete;

    yardtask(yardtask && other) noexcept
        : coroutine_(std::exchange(other.coroutine_, nullptr))
    {
    }

    yardtask & operator=(yardtask && other) noexcept
    {
        if (this != &other) {
            destroy();
            coroutine_ = std::exchange(other.coroutine_, nullptr);
        }
        return *this;
    }

    ~yardtask()
    {
        destroy();
    }

    [[nodiscard]] bool is_ready() const noexcept
    {
        return !coroutine_ || coroutine_.done();
    }

    void request_stop() noexcept
    {
        if (coroutine_)
            coroutine_.promise().frame_->request_stop();
    }

    [[nodiscard]] std::shared_ptr<yard_frame> frame() const noexcept
    {
        if (!coroutine_)
            return {};
        return coroutine_.promise().frame_;
    }

    [[nodiscard]] auto operator co_await() & noexcept
    {
        return lvalue_awaiter{coroutine_};
    }

    [[nodiscard]] auto operator co_await() && noexcept
    {
        return rvalue_awaiter{coroutine_};
    }

    [[nodiscard]] coroutine_handle handle() const noexcept
    {
        return coroutine_;
    }

private:
    bool destroy() noexcept
    {
        if (!coroutine_)
            return false;
        coroutine_.destroy();
        coroutine_ = nullptr;
        return true;
    }

    coroutine_handle coroutine_{nullptr};
};

namespace detail {

template<typename T>
inline auto yard_promise<T>::get_return_object() noexcept -> task_type
{
    return task_type{
        std::coroutine_handle<yard_promise<T>>::from_promise(*this)};
}

inline auto yard_promise<void>::get_return_object() noexcept -> task_type
{
    return task_type{
        std::coroutine_handle<yard_promise<void>>::from_promise(*this)};
}

} // namespace detail

[[nodiscard]] inline std::shared_ptr<yard_frame> yard_current_frame()
{
    return detail::current_yard_frame;
}

[[nodiscard]] inline yard_context * yard_current_context() noexcept
{
    if (!detail::current_yard_frame)
        return nullptr;
    return &detail::current_yard_frame->context;
}

[[nodiscard]] inline std::stop_token yard_stop_token() noexcept
{
    if (!detail::current_yard_frame)
        return {};
    return detail::current_yard_frame->stop_token();
}

[[nodiscard]] inline bool yard_stop_requested() noexcept
{
    return detail::current_yard_frame
           && detail::current_yard_frame->stop_requested();
}

template<typename T>
class yard_var
{
public:
    explicit yard_var(std::string key)
        : key_(std::move(key))
    {
    }

    void set(T value) const
    {
        auto * context = yard_current_context();
        if (!context)
            throw std::runtime_error{"no current yard context"};
        context->set<T>(key_, std::move(value));
    }

    [[nodiscard]] T * get_if() const
    {
        auto * context = yard_current_context();
        if (!context)
            return nullptr;
        return context->get_if<T>(key_);
    }

    [[nodiscard]] const T * get_if_const() const
    {
        auto * context = yard_current_context();
        if (!context)
            return nullptr;
        return std::as_const(*context).get_if<T>(key_);
    }

    [[nodiscard]] T get_or(T fallback) const
    {
        if (auto * value = get_if())
            return *value;
        return fallback;
    }

    [[nodiscard]] T get() const
    {
        if (auto * value = get_if())
            return *value;
        throw std::out_of_range{"yard context variable is not set"};
    }

private:
    std::string key_;
};

namespace detail {

template<typename T>
struct yard_spawn_state
{
    explicit yard_spawn_state(yardtask<T> task)
        : task(std::move(task))
    {
    }

    yardtask<T> task;
    nxt::latch done{1};
};

template<typename T>
nxt::task<> drive_yard_spawn(std::shared_ptr<yard_spawn_state<T>> state)
{
    try {
        co_await state->task;
    } catch (...) {
    }
    state->done.count_down();
}

} // namespace detail

template<typename T>
class [[nodiscard]] yard_spawned
{
public:
    explicit yard_spawned(
        std::shared_ptr<detail::yard_spawn_state<T>> state)
        : state_(std::move(state))
    {
    }

    yard_spawned(const yard_spawned &) = delete;
    yard_spawned & operator=(const yard_spawned &) = delete;

    yard_spawned(yard_spawned &&) noexcept = default;

    yard_spawned & operator=(yard_spawned && other) noexcept
    {
        if (this != &other) {
            cancel();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~yard_spawned()
    {
        cancel();
    }

    void cancel() noexcept
    {
        if (state_)
            state_->task.request_stop();
    }

    [[nodiscard]] yardtask<T> join() const
    {
        auto state = state_;
        co_await state->done;
        if constexpr (std::is_void_v<T>) {
            co_await state->task;
        } else {
            co_return co_await state->task;
        }
    }

private:
    std::shared_ptr<detail::yard_spawn_state<T>> state_;
};

template<typename T>
[[nodiscard]] yard_spawned<T>
yard_spawn(nxt::scheduler & scheduler, yardtask<T> task)
{
    auto state = std::make_shared<detail::yard_spawn_state<T>>(
        std::move(task));
    scheduler.spawn_detached(detail::drive_yard_spawn(state));
    return yard_spawned<T>{std::move(state)};
}

} // namespace nxt
