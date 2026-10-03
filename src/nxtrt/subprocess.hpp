#pragma once

#include "nxtrt/task.hpp"

#include <chrono>
#include <csignal>
#include <utility>
#include <vector>

/**
 * @namespace nxtrt::subprocess
 * Child processes as runtime tasks: spawn, wait, signal, terminate.
 *
 * `spawn_piped` starts a program with stdin from /dev/null and stdout and
 * stderr merged into one pipe; `nxtrt::pty::spawn` starts one on a
 * pseudo-terminal. Both give a child that owns a process handle: a pidfd on
 * Linux, so signals can never reach a reused pid; elsewhere the pid itself,
 * kept unreaped (kqueue `EVFILT_PROC` waits, `WNOWAIT` status reads) until
 * the handle is destroyed. Waiting suspends only the awaiting task.
 * `terminate_and_wait` is the cleanup path for cancellation. On macOS
 * piped children inherit only their stdio (`POSIX_SPAWN_CLOEXEC_DEFAULT`);
 * see `nxtrt::spawn` for the details of process creation.
 */
namespace nxtrt::subprocess {

using result = child_result;
using piped_child = nxtrt::piped_child;
using pty_child = nxtrt::pty_child;

using namespace std::chrono_literals;

/// Starts ARGV, searching PATH for ARGV[0], with the parent's environment.
///
/// The child's stdin is /dev/null and its stdout and stderr share one pipe,
/// whose read end is `piped_child::output`. Process creation runs
/// synchronously on the deck thread. Throws `errno_error` if the program
/// cannot be started (an empty ARGV is EINVAL); no child is left behind on
/// failure. The returned child owns its handle and output pipe; reading the
/// output to its end and waiting are the caller's job.
inline task<piped_child> spawn_piped(std::vector<std::string> argv)
{
    co_return co_await op::spawn_piped{std::move(argv)};
}

/// Waits for CHILD to exit and returns its status, suspending only the
/// awaiting task.
///
/// On Linux this reaps the child through its pidfd, so wait once. Elsewhere
/// the status is read without reaping, and waiting again reports the same
/// status; the handle reaps when destroyed. Cancellation cancels the wait,
/// not the child.
inline task<result> wait_child(piped_child const & child)
{
    co_return co_await op::wait_child{child.child_ref()};
}

inline task<result> wait_child(pty_child const & child)
{
    co_return co_await op::wait_child{child.child_ref()};
}

/// Sends SIGNAL to CHILD (the process only, not its process group), through
/// the pidfd on Linux.
inline task<void> signal_child(
    piped_child const & child,
    int signal = SIGTERM)
{
    co_await op::signal_child{child.child_ref(), signal};
}

inline task<void> signal_child(
    pty_child const & child,
    int signal = SIGTERM)
{
    co_await op::signal_child{child.child_ref(), signal};
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

/// Sends SIGTERM, waits up to GRACE for exit, then sends SIGKILL and waits
/// again; returns the exit status.
///
/// Runs shielded: stopping the awaiting task does not interrupt it, so it
/// is safe to await from cleanup during cancellation and always reaps (on
/// Linux) or observes the exit. Only the child process is signalled, not
/// processes it started. Do not call it on a child already reaped by
/// `wait_child` on Linux.
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

} // namespace nxtrt::subprocess
