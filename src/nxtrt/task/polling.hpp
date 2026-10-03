#pragma once

// Readiness polling raced against a timeout.
// Include nxtrt/task.hpp for the complete runtime API.

#include "nxtrt/task/concurrent.hpp"

namespace nxtrt {

namespace detail {

inline task<poll_until_result> poll_ready(op::poll wish)
{
    co_return poll_until_result{
        .events = co_await wish,
        .timed_out = false,
    };
}

inline task<poll_until_result> poll_deadline(
    std::chrono::nanoseconds duration)
{
    co_await op::timeout::after(duration);
    co_return poll_until_result{
        .events = 0,
        .timed_out = true,
    };
}

using poll_until_outcomes =
    std::tuple<outcome<poll_until_result>, outcome<poll_until_result>>;

inline poll_until_result
take_poll_until_result(poll_until_outcomes & outcomes)
{
    auto ready_result = std::move(std::get<0>(outcomes));
    auto deadline_result = std::move(std::get<1>(outcomes));

    if (ready_result)
        return std::move(*ready_result);
    if (deadline_result)
        return std::move(*deadline_result);

    if (!is_operation_cancelled(ready_result.error()))
        rethrow(ready_result.error());
    if (!is_operation_cancelled(deadline_result.error()))
        rethrow(deadline_result.error());
    rethrow(ready_result.error());
}

} // namespace detail

/// Wait until an fd is ready or a timeout expires.
///
/// This composes ordinary `op::poll` and `op::timeout` wishes in a fixed
/// group. Completion stops the other task; the group drains the
/// losing wish before the outcome is returned.
[[nodiscard]] inline task<poll_until_result> poll_until_after(
    int fd,
    short events,
    std::chrono::nanoseconds timeout)
{
    auto outcomes = co_await settle(
        std::tuple{
            detail::poll_ready(op::poll{fd, events}),
            detail::poll_deadline(timeout),
        },
        first_completion_group{});
    co_return detail::take_poll_until_result(outcomes);
}

} // namespace nxtrt
