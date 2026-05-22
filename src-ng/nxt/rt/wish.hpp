#pragma once

#include "nxt/rt/exceptions.hpp"

#include <coroutine>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <linux/time_types.h>
#include <memory>
#include <poll.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <utility>

namespace nxt::rt {

class deck;
class wand;

namespace detail {
struct promise_base;
}

using wait_token = std::uint64_t;

inline __kernel_timespec as_kernel_timespec(std::chrono::nanoseconds duration)
{
    if (duration < std::chrono::nanoseconds::zero())
        duration = std::chrono::nanoseconds::zero();

    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
    return __kernel_timespec{
        .tv_sec = seconds.count(),
        .tv_nsec = nanoseconds.count(),
    };
}

/// A suspended coroutine parked inside a wand.
///
/// Operation-specific wands store these records by token. When the platform
/// completion arrives, `resume()` puts the task back onto the deck.
struct parked_task
{
    void resume(deck & d) const;

    std::coroutine_handle<> handle;
    detail::promise_base * promise = nullptr;
};

template<typename T>
class waiter;

template<typename T>
class wait_state
{
public:
    using stored_type = std::remove_cv_t<T>;

    void set_value(T value)
    {
        value_.emplace(std::move(value));
    }

    void set_exception(std::exception_ptr exception) noexcept
    {
        exception_ = exception;
    }

    T take()
    {
        if (exception_)
            rethrow(exception_);
        if (!value_)
            throw runtime_error{"nxt::rt waiter result was never set"};
        return std::move(*value_);
    }

private:
    std::optional<stored_type> value_;
    std::exception_ptr exception_;
};

template<>
class wait_state<void>
{
public:
    void set_value() noexcept
    {
        done_ = true;
    }

    void set_exception(std::exception_ptr exception) noexcept
    {
        exception_ = exception;
    }

    void take()
    {
        if (exception_)
            rethrow(exception_);
        if (!done_)
            throw runtime_error{"nxt::rt waiter result was never set"};
    }

private:
    bool done_ = false;
    std::exception_ptr exception_;
};

/// Typed waiter returned by a wand after preparing an operation.
template<typename T>
class waiter
{
public:
    using result_type = T;

    waiter() = default;
    waiter(
        wand & source,
        wait_token token,
        std::shared_ptr<wait_state<T>> state) noexcept
        : source_(&source)
        , token_(token)
        , state_(std::move(state))
    {}

    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> awaiting) const;
    T await_resume()
    {
        if (state_ == nullptr)
            throw runtime_error{"nxt::rt waiter has no result state"};
        return state_->take();
    }

    [[nodiscard]] wait_token token() const noexcept
    {
        return token_;
    }

    [[nodiscard]] std::shared_ptr<wait_state<T>> state() const noexcept
    {
        return state_;
    }

private:
    wand * source_ = nullptr;
    wait_token token_ = 0;
    std::shared_ptr<wait_state<T>> state_;
};

template<>
inline void waiter<void>::await_resume()
{
    if (state_ == nullptr)
        throw runtime_error{"nxt::rt waiter has no result state"};
    state_->take();
}

/// Closed operation type for deterministic/manual tests.
///
/// This is deliberately more like a tiny SQE recipe than a generic variant:
/// the operation owns its input parameters and names its result type.
struct manual_wish
{
    using result_type = void;

    wait_token token = 0;

    waiter<void> operator co_await() const;
};

struct openat_wish
{
    using result_type = int;

    int dirfd = AT_FDCWD;
    std::string path;
    int flags = O_RDONLY;
    mode_t mode = 0;

    waiter<int> operator co_await() const;
};

struct statx_wish
{
    using result_type = struct statx;

    int dirfd = AT_FDCWD;
    std::string path;
    int flags = AT_SYMLINK_NOFOLLOW;
    unsigned mask = STATX_BASIC_STATS;
    struct statx result{};

    waiter<struct statx> operator co_await() const;
};

struct getdents64_wish
{
    using result_type = std::size_t;

    int fd = -1;
    std::span<std::byte> buffer;

    waiter<std::size_t> operator co_await() const;
};

struct read_some_wish
{
    using result_type = std::size_t;

    int fd = -1;
    std::span<std::byte> buffer;
    off_t offset = -1;

    waiter<std::size_t> operator co_await() const;
};

struct recv_some_wish
{
    using result_type = std::size_t;

    int fd = -1;
    std::span<std::byte> buffer;
    int flags = 0;

    waiter<std::size_t> operator co_await() const;
};

struct send_some_wish
{
    using result_type = std::size_t;

    int fd = -1;
    std::span<const std::byte> buffer;
    int flags = 0;

    waiter<std::size_t> operator co_await() const;
};

struct connect_wish
{
    using result_type = void;

    int fd = -1;
    sockaddr_storage address{};
    socklen_t address_size = 0;

    static connect_wish from(
        int fd,
        sockaddr const * address,
        socklen_t address_size)
    {
        if (address_size > sizeof(sockaddr_storage))
            throw runtime_error{"connect address is too large"};

        auto wish = connect_wish{
            .fd = fd,
            .address = {},
            .address_size = address_size,
        };
        std::memcpy(&wish.address, address, address_size);
        return wish;
    }

    [[nodiscard]] sockaddr const * sockaddr_ptr() const noexcept
    {
        return reinterpret_cast<sockaddr const *>(&address);
    }

    waiter<void> operator co_await() const;
};

struct poll_wish
{
    using result_type = int;

    int fd = -1;
    short events = 0;

    waiter<int> operator co_await() const;
};

struct timeout_wish
{
    using result_type = void;

    __kernel_timespec duration{};

    static timeout_wish after(std::chrono::nanoseconds duration)
    {
        return timeout_wish{
            .duration = as_kernel_timespec(duration),
        };
    }

    waiter<void> operator co_await() const;
};

struct poll_until_result
{
    int events = 0;
    bool timed_out = false;
};

struct poll_until_wish
{
    using result_type = poll_until_result;

    int fd = -1;
    short events = 0;
    __kernel_timespec timeout{};

    static poll_until_wish after(
        int fd,
        short events,
        std::chrono::nanoseconds timeout)
    {
        return poll_until_wish{
            .fd = fd,
            .events = events,
            .timeout = as_kernel_timespec(timeout),
        };
    }

    waiter<poll_until_result> operator co_await() const;
};

/// Backend interface for staged platform/event-loop machinery.
///
/// `prepare()` is called synchronously while a coroutine is running. It can
/// allocate backend state, stage submission records, and return a typed waiter.
/// The waiter parks the coroutine at `await_suspend()`. After a deck round,
/// `wave()` lets the wand submit whatever it staged during that round.
class wand
{
public:
    virtual ~wand() = default;

    virtual waiter<void> prepare(
        deck & d,
        detail::promise_base & promise,
        manual_wish wish) = 0;

    virtual waiter<int> prepare(
        deck & d,
        detail::promise_base & promise,
        openat_wish wish) = 0;

    virtual waiter<struct statx> prepare(
        deck & d,
        detail::promise_base & promise,
        statx_wish wish) = 0;

    virtual waiter<std::size_t> prepare(
        deck & d,
        detail::promise_base & promise,
        getdents64_wish wish) = 0;

    virtual waiter<std::size_t> prepare(
        deck & d,
        detail::promise_base & promise,
        read_some_wish wish) = 0;

    virtual waiter<std::size_t> prepare(
        deck & d,
        detail::promise_base & promise,
        recv_some_wish wish) = 0;

    virtual waiter<std::size_t> prepare(
        deck & d,
        detail::promise_base & promise,
        send_some_wish wish) = 0;

    virtual waiter<void> prepare(
        deck & d,
        detail::promise_base & promise,
        connect_wish wish) = 0;

    virtual waiter<int> prepare(
        deck & d,
        detail::promise_base & promise,
        poll_wish wish) = 0;

    virtual waiter<void> prepare(
        deck & d,
        detail::promise_base & promise,
        timeout_wish wish) = 0;

    virtual waiter<poll_until_result> prepare(
        deck & d,
        detail::promise_base & promise,
        poll_until_wish wish) = 0;

    virtual void suspend(wait_token token, parked_task task) = 0;
    virtual void cancel(wait_token token) = 0;
    virtual void wave(deck & d) = 0;
};

} // namespace nxt::rt
