#pragma once

#include "nxtrt/task.hpp"

#include <chrono>
#include <csignal>
#include <utility>
#include <vector>

namespace nxtrt::subprocess {

#if defined(__linux__)
using result = child_result;
using piped_child = nxtrt::piped_child;
using pty_child = nxtrt::pty_child;

using namespace std::chrono_literals;

inline task<piped_child> spawn_piped(std::vector<std::string> argv)
{
    co_return co_await op::spawn_piped{std::move(argv)};
}

inline task<result> wait_child(piped_child const & child)
{
    co_return co_await op::wait_child{child.pid_fd()};
}

inline task<result> wait_child(pty_child const & child)
{
    co_return co_await op::wait_child{child.pid_fd()};
}

inline task<void> signal_child(
    piped_child const & child,
    int signal = SIGTERM)
{
    co_await op::signal_child{child.pid_fd(), signal};
}

inline task<void> signal_child(
    pty_child const & child,
    int signal = SIGTERM)
{
    co_await op::signal_child{child.pid_fd(), signal};
}

namespace detail {

template<typename Child>
inline task<result> terminate_and_wait_impl(
    Child const & child,
    std::chrono::milliseconds grace)
{
    co_await signal_child(child, SIGTERM);
    try {
        co_return co_await with_timeout(grace, wait_child(child));
    } catch (const timeout_error &) {
    }

    co_await signal_child(child, SIGKILL);
    co_return co_await wait_child(child);
}

} // namespace detail

inline task<result> terminate_and_wait(
    piped_child const & child,
    std::chrono::milliseconds grace = 500ms)
{
    co_return co_await shield(detail::terminate_and_wait_impl(child, grace));
}

inline task<result> terminate_and_wait(
    pty_child const & child,
    std::chrono::milliseconds grace = 500ms)
{
    co_return co_await shield(detail::terminate_and_wait_impl(child, grace));
}
#endif

} // namespace nxtrt::subprocess
