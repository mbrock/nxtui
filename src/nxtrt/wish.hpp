#pragma once

#include "nxtrt/exceptions.hpp"
#include "nxt/unique-fd.hpp"

#include <csignal>
#include <coroutine>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(__linux__)
#include <linux/time_types.h>
#endif

namespace nxtrt {

class deck;
class wand;
class uring_submission;

namespace detail {
struct promise_base;
}

using wait_token = std::uint64_t;

#if defined(__linux__)
using kernel_timespec = __kernel_timespec;
#else
struct kernel_timespec
{
    std::int64_t tv_sec = 0;
    std::int64_t tv_nsec = 0;
};
#endif

inline kernel_timespec as_kernel_timespec(std::chrono::nanoseconds duration)
{
    if (duration < std::chrono::nanoseconds::zero())
        duration = std::chrono::nanoseconds::zero();

    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
    return kernel_timespec{
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
            throw runtime_error{"nxtrt waiter result was never set"};
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
            throw runtime_error{"nxtrt waiter result was never set"};
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
        std::shared_ptr<wait_state<T>> state,
        std::string description = {}) noexcept
        : source_(&source)
        , token_(token)
        , state_(std::move(state))
        , description_(std::move(description))
    {}

    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> awaiting) const;
    T await_resume()
    {
        if (state_ == nullptr)
            throw runtime_error{"nxtrt waiter has no result state"};
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
    std::string description_;
};

template<>
inline void waiter<void>::await_resume()
{
    if (state_ == nullptr)
        throw runtime_error{"nxtrt waiter has no result state"};
    state_->take();
}

struct poll_until_result
{
    int events = 0;
    bool timed_out = false;
};

#if defined(__linux__)
using statx_result = struct statx;

struct child_result
{
    pid_t pid = -1;
    int code = 0;
    bool exited = false;
    int exit_code = 0;
    bool signaled = false;
    int signal = 0;
};

struct piped_child
{
    pid_t pid = -1;
    nxt::unique_fd pidfd;
    nxt::unique_fd output;

    [[nodiscard]] int pid_fd() const noexcept
    {
        return pidfd.get();
    }

    [[nodiscard]] int output_fd() const noexcept
    {
        return output.get();
    }
};

struct pty_child
{
    pid_t pid = -1;
    nxt::unique_fd pidfd;
    nxt::unique_fd master;

    [[nodiscard]] int pid_fd() const noexcept
    {
        return pidfd.get();
    }

    [[nodiscard]] int master_fd() const noexcept
    {
        return master.get();
    }
};
#endif

namespace op {

/// Closed operation type for deterministic/manual tests.
///
/// This is deliberately more like a tiny SQE recipe than a generic variant:
/// the operation owns its input parameters and names its result type.
struct manual
{
    using result_type = void;
    static constexpr std::string_view name = "manual";

    wait_token token = 0;

    bool stage_uring(uring_submission & submission);
    waiter<void> operator co_await() const;
};

struct openat
{
    using result_type = int;
    static constexpr std::string_view name = "openat";

    int dirfd = AT_FDCWD;
    std::string path;
    int flags = O_RDONLY;
    mode_t mode = 0;

    bool stage_uring(uring_submission & submission);
    waiter<int> operator co_await() const;
};

#if defined(__linux__)
struct statx
{
    using result_type = statx_result;
    static constexpr std::string_view name = "statx";

    int dirfd = AT_FDCWD;
    std::string path;
    int flags = AT_SYMLINK_NOFOLLOW;
    unsigned mask = STATX_BASIC_STATS;
    statx_result result{};

    bool stage_uring(uring_submission & submission);
    waiter<statx_result> operator co_await() const;
};

struct getdents64
{
    using result_type = std::size_t;
    static constexpr std::string_view name = "getdents64";

    int fd = -1;
    std::span<std::byte> buffer;

    bool stage_uring(uring_submission & submission);
    waiter<std::size_t> operator co_await() const;
};

struct spawn_piped
{
    using result_type = piped_child;
    static constexpr std::string_view name = "spawn-piped";

    std::vector<std::string> argv;
    std::shared_ptr<piped_child> child = std::make_shared<piped_child>();

    bool stage_uring(uring_submission & submission);
    waiter<piped_child> operator co_await() const;
};

struct spawn_pty
{
    using result_type = pty_child;
    static constexpr std::string_view name = "spawn-pty";

    std::vector<std::string> argv;
    std::size_t columns = 80;
    std::size_t rows = 24;
    std::shared_ptr<pty_child> child = std::make_shared<pty_child>();

    bool stage_uring(uring_submission & submission);
    waiter<pty_child> operator co_await() const;
};

struct wait_child
{
    using result_type = child_result;
    static constexpr std::string_view name = "wait-child";

    int pidfd = -1;
    siginfo_t info{};

    bool stage_uring(uring_submission & submission);
    waiter<child_result> operator co_await() const;
};

struct signal_child
{
    using result_type = void;
    static constexpr std::string_view name = "signal-child";

    int pidfd = -1;
    int signal = SIGTERM;

    bool stage_uring(uring_submission & submission);
    waiter<void> operator co_await() const;
};
#endif

struct read_some
{
    using result_type = std::size_t;
    static constexpr std::string_view name = "read";

    int fd = -1;
    std::span<std::byte> buffer;
    off_t offset = -1;

    bool stage_uring(uring_submission & submission);
    waiter<std::size_t> operator co_await() const;
};

struct write_some
{
    using result_type = std::size_t;
    static constexpr std::string_view name = "write";

    int fd = -1;
    std::span<const std::byte> buffer;
    off_t offset = -1;

    bool stage_uring(uring_submission & submission);
    waiter<std::size_t> operator co_await() const;
};

struct recv_some
{
    using result_type = std::size_t;
    static constexpr std::string_view name = "recv";

    int fd = -1;
    std::span<std::byte> buffer;
    int flags = 0;

    bool stage_uring(uring_submission & submission);
    waiter<std::size_t> operator co_await() const;
};

struct send_some
{
    using result_type = std::size_t;
    static constexpr std::string_view name = "send";

    int fd = -1;
    std::span<const std::byte> buffer;
    int flags = 0;

    bool stage_uring(uring_submission & submission);
    waiter<std::size_t> operator co_await() const;
};

struct connect
{
    using result_type = void;
    static constexpr std::string_view name = "connect";

    int fd = -1;
    sockaddr_storage address{};
    socklen_t address_size = 0;

    static connect from(
        int fd,
        sockaddr const * address,
        socklen_t address_size)
    {
        if (address_size > sizeof(sockaddr_storage))
            throw runtime_error{"connect address is too large"};

        auto op = connect{
            .fd = fd,
            .address = {},
            .address_size = address_size,
        };
        std::memcpy(&op.address, address, address_size);
        return op;
    }

    [[nodiscard]] sockaddr const * sockaddr_ptr() const noexcept
    {
        return reinterpret_cast<sockaddr const *>(&address);
    }

    bool stage_uring(uring_submission & submission);
    waiter<void> operator co_await() const;
};

struct poll
{
    using result_type = int;
    static constexpr std::string_view name = "poll";

    int fd = -1;
    short events = 0;

    bool stage_uring(uring_submission & submission);
    waiter<int> operator co_await() const;
};

struct timeout
{
    using result_type = void;
    static constexpr std::string_view name = "timeout";

    kernel_timespec duration{};

    static timeout after(std::chrono::nanoseconds duration)
    {
        return timeout{
            .duration = as_kernel_timespec(duration),
        };
    }

    bool stage_uring(uring_submission & submission);
    waiter<void> operator co_await() const;
};

struct poll_until
{
    using result_type = poll_until_result;
    static constexpr std::string_view name = "poll-until";

    int fd = -1;
    short events = 0;
    kernel_timespec timeout{};

    static poll_until after(
        int fd,
        short events,
        std::chrono::nanoseconds timeout)
    {
        return poll_until{
            .fd = fd,
            .events = events,
            .timeout = as_kernel_timespec(timeout),
        };
    }

    bool stage_uring(uring_submission & submission);
    waiter<poll_until_result> operator co_await() const;
};

} // namespace op

using wish_variant = std::variant<
    op::manual,
    op::openat,
#if defined(__linux__)
    op::statx,
    op::getdents64,
    op::spawn_piped,
    op::spawn_pty,
    op::wait_child,
    op::signal_child,
#endif
    op::read_some,
    op::write_some,
    op::recv_some,
    op::send_some,
    op::connect,
    op::poll,
    op::timeout,
    op::poll_until>;

namespace detail {

struct prepared_wish
{
    wish_variant wish;
    std::shared_ptr<void> state;
};

inline std::string describe_wish(const op::manual & wish)
{
    return "manual token " + std::to_string(wish.token);
}

inline std::string describe_wish(const op::openat & wish)
{
    return "openat " + wish.path;
}

#if defined(__linux__)
inline std::string describe_wish(const op::statx & wish)
{
    return "statx " + wish.path;
}

inline std::string describe_wish(const op::getdents64 & wish)
{
    return "getdents64 fd " + std::to_string(wish.fd)
        + " bytes " + std::to_string(wish.buffer.size());
}

inline std::string describe_wish(const op::spawn_piped & wish)
{
    auto command = wish.argv.empty() ? std::string{} : wish.argv.front();
    return "spawn-piped argc " + std::to_string(wish.argv.size())
        + (command.empty() ? std::string{} : " command " + command);
}

inline std::string describe_wish(const op::spawn_pty & wish)
{
    auto command = wish.argv.empty() ? std::string{} : wish.argv.front();
    return "spawn-pty argc " + std::to_string(wish.argv.size())
        + (command.empty() ? std::string{} : " command " + command);
}

inline std::string describe_wish(const op::wait_child & wish)
{
    return "wait-child pidfd " + std::to_string(wish.pidfd);
}

inline std::string describe_wish(const op::signal_child & wish)
{
    return "signal-child pidfd " + std::to_string(wish.pidfd)
        + " signal " + std::to_string(wish.signal);
}
#endif

inline std::string describe_wish(const op::read_some & wish)
{
    return "read fd " + std::to_string(wish.fd)
        + " bytes " + std::to_string(wish.buffer.size());
}

inline std::string describe_wish(const op::write_some & wish)
{
    return "write fd " + std::to_string(wish.fd)
        + " bytes " + std::to_string(wish.buffer.size());
}

inline std::string describe_wish(const op::recv_some & wish)
{
    return "recv fd " + std::to_string(wish.fd)
        + " bytes " + std::to_string(wish.buffer.size());
}

inline std::string describe_wish(const op::send_some & wish)
{
    return "send fd " + std::to_string(wish.fd)
        + " bytes " + std::to_string(wish.buffer.size());
}

inline std::string describe_wish(const op::connect & wish)
{
    return "connect fd " + std::to_string(wish.fd);
}

inline std::string describe_wish(const op::poll & wish)
{
    return "poll fd " + std::to_string(wish.fd)
        + " events " + std::to_string(wish.events);
}

inline std::string describe_wish(const op::timeout & wish)
{
    auto millis = wish.duration.tv_sec * 1000 + wish.duration.tv_nsec / 1000000;
    return "timeout " + std::to_string(millis) + "ms";
}

inline std::string describe_wish(const op::poll_until & wish)
{
    return "poll-until fd " + std::to_string(wish.fd)
        + " events " + std::to_string(wish.events);
}

} // namespace detail

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

    template<typename Wish>
    waiter<typename Wish::result_type> prepare(
        deck & d,
        detail::promise_base & promise,
        Wish wish)
    {
        using result_type = typename Wish::result_type;
        auto state = std::make_shared<wait_state<result_type>>();
        auto description = detail::describe_wish(wish);
        auto token = prepare_wish(
            d,
            promise,
            detail::prepared_wish{
                .wish = wish_variant{std::move(wish)},
                .state = state,
            });
        return waiter<result_type>{
            *this,
            token,
            state,
            std::move(description)};
    }

    virtual void suspend(wait_token token, parked_task task) = 0;
    virtual void cancel(wait_token token) = 0;
    virtual void wave(deck & d) = 0;

protected:
    virtual wait_token prepare_wish(
        deck & d,
        detail::promise_base & promise,
        detail::prepared_wish wish) = 0;
};

} // namespace nxtrt
