#pragma once

#include "nxtrt/debug.hpp"
#include "nxtrt/exceptions.hpp"
#include "nxtrt/trace.hpp"
#include "nxt/unique-fd.hpp"

#include <algorithm>
#include <array>
#include <concepts>
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
#include <ranges>
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
class urge;

template<typename T>
class urge_state
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
            throw runtime_error{"nxtrt urge result was never set"};
        return std::move(*value_);
    }

private:
    std::optional<stored_type> value_;
    std::exception_ptr exception_;
};

template<>
class urge_state<void>
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
            throw runtime_error{"nxtrt urge result was never set"};
    }

private:
    bool done_ = false;
    std::exception_ptr exception_;
};

/// Typed urge returned by a wand after preparing an operation.
template<typename T>
class urge
{
public:
    using result_type = T;

    urge() = default;
    urge(
        wand & source,
        wait_token token,
        std::shared_ptr<urge_state<T>> state,
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
            throw runtime_error{"nxtrt urge has no result state"};
        return state_->take();
    }

    [[nodiscard]] wait_token token() const noexcept
    {
        return token_;
    }

    [[nodiscard]] std::shared_ptr<urge_state<T>> state() const noexcept
    {
        return state_;
    }

private:
    wand * source_ = nullptr;
    wait_token token_ = 0;
    std::shared_ptr<urge_state<T>> state_;
    std::string description_;
};

template<>
inline void urge<void>::await_resume()
{
    if (state_ == nullptr)
        throw runtime_error{"nxtrt urge has no result state"};
    state_->take();
}

/// Result of waiting for file-descriptor readiness until a deadline.
struct poll_until_result
{
    /// Poll revents when readiness wins; zero when the deadline wins.
    int events = 0;
    /// True when the deadline completed before fd readiness.
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

struct wish_arg
{
    using value_type =
        std::variant<std::intmax_t, std::uintmax_t, std::string_view>;

    std::string_view name;
    value_type value;

    template<std::integral T>
    constexpr wish_arg(std::string_view name, T value) noexcept
        : name(name)
        , value(make_value(value))
    {}

    constexpr wish_arg(std::string_view name, std::string_view value) noexcept
        : name(name)
        , value(value)
    {}

private:
    template<std::integral T>
    static constexpr value_type make_value(T value) noexcept
    {
        if constexpr (std::is_signed_v<T>)
            return static_cast<std::intmax_t>(value);
        else
            return static_cast<std::uintmax_t>(value);
    }
};

template<std::size_t N>
struct fixed_string
{
    char value[N]{};

    constexpr fixed_string(char const (&text)[N]) noexcept
    {
        std::copy_n(text, N, value);
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return std::string_view{value, N - 1};
    }
};

template<typename Result, fixed_string Name>
struct wish
{
    using result_type = Result;
    static constexpr auto fixed_name = Name;
    static constexpr auto name = fixed_name.view();
};

template<typename... Args>
constexpr auto wish_args(Args &&... args)
{
    if constexpr (sizeof...(Args) == 0)
        return std::array<wish_arg, 0>{};
    else
        return std::array{wish_arg{std::forward<Args>(args)}...};
}

inline auto no_args()
{
    return wish_args();
}

inline auto fd_args(int fd)
{
    return wish_args(wish_arg{"fd", fd});
}

inline auto fd_bytes_args(int fd, std::size_t bytes)
{
    return wish_args(
        wish_arg{"fd", fd},
        wish_arg{"bytes", bytes});
}

inline auto path_args(std::string const & path)
{
    return wish_args(wish_arg{"path", std::string_view{path}});
}

inline auto pidfd_args(int pidfd)
{
    return wish_args(wish_arg{"pidfd", pidfd});
}

template<std::ranges::input_range Args>
std::string format_wish_args(Args const & args)
{
    auto out = std::string{};
    auto first = true;
    for (auto const & arg : args) {
        if (!first)
            out += ' ';
        first = false;
        out += arg.name;
        out += '=';
        std::visit(
            [&](auto const & value) {
                if constexpr (std::same_as<
                                  std::remove_cvref_t<decltype(value)>,
                                  std::string_view>)
                    out += value;
                else
                    out += std::to_string(value);
            },
            arg.value);
    }
    return out;
}

template<typename Wish>
std::string describe_wish(Wish const & wish)
{
    auto args = wish.args();
    if (std::ranges::empty(args))
        return std::string{Wish::name};
    return std::string{Wish::name}
        + " "
        + format_wish_args(args);
}

template<typename Wish>
void trace_wish(Wish const & wish)
{
    trace("{}", describe_wish(wish));
}

template<typename Wish>
concept awaitable_wish =
    requires(Wish const & wish) {
        typename Wish::result_type;
        { Wish::name } -> std::convertible_to<std::string_view>;
        wish.args();
    };

template<awaitable_wish Wish>
urge<typename Wish::result_type> operator co_await(Wish const & wish);

/// Closed operation type for deterministic/manual tests.
///
/// This is deliberately more like a tiny SQE recipe than a generic variant:
/// the operation owns its input parameters and names its result type.
struct manual : wish<void, "manual">
{
    wait_token token = 0;

    auto args() const
    {
        return wish_args(wish_arg{"token", token});
    }

};

struct openat : wish<int, "openat">
{
    int dirfd = AT_FDCWD;
    std::string path;
    int flags = O_RDONLY;
    mode_t mode = 0;

    auto args() const
    {
        return path_args(path);
    }

};

#if defined(__linux__)
struct statx : wish<statx_result, "statx">
{
    int dirfd = AT_FDCWD;
    std::string path;
    int flags = AT_SYMLINK_NOFOLLOW;
    unsigned mask = STATX_BASIC_STATS;
    statx_result result{};

    auto args() const
    {
        return path_args(path);
    }

};

struct getdents64 : wish<std::size_t, "getdents64">
{
    int fd = -1;
    std::span<std::byte> buffer;

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }

};

struct spawn_piped : wish<piped_child, "spawn-piped">
{
    std::vector<std::string> argv;
    std::shared_ptr<piped_child> child = std::make_shared<piped_child>();

    auto args() const
    {
        return wish_args(wish_arg{"argv", argv.size()});
    }

};

struct spawn_pty : wish<pty_child, "spawn-pty">
{
    std::vector<std::string> argv;
    std::size_t columns = 80;
    std::size_t rows = 24;
    std::shared_ptr<pty_child> child = std::make_shared<pty_child>();

    auto args() const
    {
        return wish_args(wish_arg{"argv", argv.size()});
    }

};

struct wait_child : wish<child_result, "wait-child">
{
    int pidfd = -1;
    siginfo_t info{};

    auto args() const
    {
        return pidfd_args(pidfd);
    }

};

struct signal_child : wish<void, "signal-child">
{
    int pidfd = -1;
    int signal = SIGTERM;

    auto args() const
    {
        return wish_args(
            wish_arg{"pidfd", pidfd},
            wish_arg{"signal", signal});
    }

};
#endif

struct read_some : wish<std::size_t, "read">
{
    int fd = -1;
    std::span<std::byte> buffer;
    off_t offset = -1;

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }

};

struct write_some : wish<std::size_t, "write">
{
    int fd = -1;
    std::span<const std::byte> buffer;
    off_t offset = -1;

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }

};

struct recv_some : wish<std::size_t, "recv">
{
    int fd = -1;
    std::span<std::byte> buffer;
    int flags = 0;

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }

};

struct send_some : wish<std::size_t, "send">
{
    int fd = -1;
    std::span<const std::byte> buffer;
    int flags = 0;

    auto args() const
    {
        return fd_bytes_args(fd, buffer.size());
    }

};

struct connect : wish<void, "connect">
{
    int fd = -1;
    sockaddr_storage address{};
    socklen_t address_size = 0;

    auto args() const
    {
        return fd_args(fd);
    }

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

};

/// Accept one connection from a listening socket.
///
/// The accepted file descriptor is returned as the wish result. The caller owns
/// it immediately and should wrap it in an RAII file descriptor type.
struct accept : wish<int, "accept">
{
    int fd = -1;
    int flags = 0;

    auto args() const
    {
        return fd_args(fd);
    }

};

struct poll : wish<int, "poll">
{
    int fd = -1;
    short events = 0;

    auto args() const
    {
        return wish_args(
            wish_arg{"fd", fd},
            wish_arg{"events", events});
    }

};

struct timeout : wish<void, "timeout">
{
    kernel_timespec duration{};

    auto args() const
    {
        return no_args();
    }

    static timeout after(std::chrono::nanoseconds duration)
    {
        return timeout{
            .duration = as_kernel_timespec(duration),
        };
    }

};

/// Backend-specific fused poll/deadline wish.
///
/// Portable runtime code should prefer `poll_until_after`, which composes a
/// poll wish and timeout wish and lets ordinary task racing choose the winner.
struct poll_until : wish<poll_until_result, "poll-until">
{
    int fd = -1;
    short events = 0;
    kernel_timespec timeout{};

    auto args() const
    {
        return wish_args(
            wish_arg{"fd", fd},
            wish_arg{"events", events});
    }

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
    op::accept,
    op::poll,
    op::timeout,
    op::poll_until>;

namespace detail {

struct prepared_wish
{
    wish_variant wish;
    std::shared_ptr<void> state;
};

template<typename Wish>
[[nodiscard]] inline std::string describe_wish_for_urge(const Wish & wish)
{
    if constexpr (debug::describe_wishes)
        return op::describe_wish(wish);
    else
        return {};
}

} // namespace detail

/// Backend interface for staged platform/event-loop machinery.
///
/// `prepare()` is called synchronously while a coroutine is running. It can
/// allocate backend state, stage submission records, and return a typed urge.
/// The urge parks the coroutine at `await_suspend()`. After a deck round,
/// `wave()` lets the wand submit whatever it staged during that round.
class wand
{
public:
    virtual ~wand() = default;

    template<typename Wish>
    urge<typename Wish::result_type> prepare(
        deck & d,
        detail::promise_base & promise,
        Wish wish)
    {
        using result_type = typename Wish::result_type;
        auto state = std::make_shared<urge_state<result_type>>();
        auto description = detail::describe_wish_for_urge(wish);
        auto token = prepare_wish(
            d,
            promise,
            detail::prepared_wish{
                .wish = wish_variant{std::move(wish)},
                .state = state,
            });
        return urge<result_type>{
            *this,
            token,
            state,
            std::move(description)
        };
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
