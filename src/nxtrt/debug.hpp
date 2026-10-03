#pragma once

#include "nxtrt/ids.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef NXT_RT_DESCRIBE_WISHES
#define NXT_RT_DESCRIBE_WISHES 0
#endif

/**
 * @namespace nxtrt::debug
 * Runtime dump of ready tasks and parked wishes, triggered by a signal.
 *
 * Call `install_signal_dump()` (the `nxtrt::runtime` constructor does) and
 * send the process SIGUSR1: at its next round boundary, a running deck
 * prints to stderr its ready task ids and every parked wish with its task,
 * coin, and how long it has waited. The parked-wish registry is
 * process-wide and mutex-protected. Wish descriptions are recorded only
 * when the build defines `NXT_RT_DESCRIBE_WISHES=1`.
 */
namespace nxtrt::debug {

/// Whether parked waits record a text description of their wish
/// (`NXT_RT_DESCRIBE_WISHES`, off by default).
inline constexpr bool describe_wishes = NXT_RT_DESCRIBE_WISHES != 0;


/// One parked wish as recorded for the runtime dump.
struct wait_snapshot
{
    task_id task;
    std::uint64_t token = 0;
    std::chrono::steady_clock::time_point parked_at;
    std::string wish;
};

namespace detail {

inline std::mutex waits_mutex;
inline std::vector<wait_snapshot> waits;
inline volatile std::sig_atomic_t dump_requested = 0;

inline void signal_handler(int) noexcept
{
    dump_requested = 1;
}

} // namespace detail

/// Installs a process-wide handler for `signal` that requests a runtime
/// dump. The handler only sets a flag; a deck prints the dump at its next
/// round boundary. Replaces any existing handler for that signal.
inline void install_signal_dump(int signal = SIGUSR1)
{
    struct sigaction action {};
    action.sa_handler = detail::signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(signal, &action, nullptr);
}

/// Returns true once per received dump signal, clearing the request.
[[nodiscard]] inline bool consume_signal_dump_request() noexcept
{
    if (detail::dump_requested == 0)
        return false;
    detail::dump_requested = 0;
    return true;
}

/// Records that `task` is parked on wish `token`; called by urges.
inline void park_task(task_id task, std::uint64_t token, std::string wish)
{
    auto parked_at = std::chrono::steady_clock::now();
    auto lock = std::scoped_lock{detail::waits_mutex};
    for (auto & wait : detail::waits) {
        if (wait.task == task) {
            wait = wait_snapshot{
                .task = task,
                .token = token,
                .parked_at = parked_at,
                .wish = std::move(wish),
            };
            return;
        }
    }
    detail::waits.push_back(
        wait_snapshot{
            .task = task,
            .token = token,
            .parked_at = parked_at,
            .wish = std::move(wish),
        });
}

[[nodiscard]] inline std::string parked_wish_description(
    const std::string & wish)
{
    if constexpr (describe_wishes)
        return wish;
    else
        return {};
}

/// Removes `task` from the parked-wish registry; called by `need::resume`.
inline void unpark_task(task_id task)
{
    auto lock = std::scoped_lock{detail::waits_mutex};
    std::erase_if(detail::waits, [task](const wait_snapshot & wait) {
        return wait.task == task;
    });
}

/// Copies the current parked-wish registry.
[[nodiscard]] inline std::vector<wait_snapshot> snapshot_waits()
{
    auto lock = std::scoped_lock{detail::waits_mutex};
    return detail::waits;
}

inline std::string format_duration(std::chrono::steady_clock::duration duration)
{
    using namespace std::chrono;
    if (duration < steady_clock::duration::zero())
        duration = steady_clock::duration::zero();

    auto micros = duration_cast<microseconds>(duration).count();
    if (micros < 1000)
        return std::to_string(micros) + "us";

    auto millis = duration_cast<milliseconds>(duration).count();
    if (millis < 1000)
        return std::to_string(millis) + "ms";

    auto seconds =
        duration_cast<std::chrono::duration<double>>(duration).count();
    if (seconds < 10.0) {
        auto tenths = static_cast<long long>(seconds * 10.0);
        return std::to_string(tenths / 10)
            + "."
            + std::to_string(tenths % 10)
            + "s";
    }

    return std::to_string(duration_cast<std::chrono::seconds>(duration).count())
        + "s";
}

/// Formats the runtime dump text from parked waits and ready task ids.
[[nodiscard]] inline std::string format_runtime_dump(
    std::vector<wait_snapshot> waits,
    std::vector<task_id> ready_tasks)
{
    auto now = std::chrono::steady_clock::now();
    auto out = std::ostringstream{};
    out << "[nxtrt] runtime dump\n";
    out << "  ready tasks: " << ready_tasks.size();
    if (!ready_tasks.empty()) {
        out << " (";
        for (auto i = std::size_t{}; i < ready_tasks.size(); ++i) {
            if (i != 0)
                out << ", ";
            out << ready_tasks[i].value;
        }
        out << ")";
    }
    out << "\n";

    out << "  parked wishes: " << waits.size() << "\n";
    for (auto const & wait : waits) {
        out << "    task " << wait.task.value
            << " token " << wait.token
            << " parked " << format_duration(now - wait.parked_at)
            << "  " << wait.wish << "\n";
    }
    return out.str();
}

/// Writes `format_runtime_dump(...)` to stderr.
inline void print_runtime_dump(
    std::vector<wait_snapshot> waits,
    std::vector<task_id> ready_tasks)
{
    std::cerr << "\n"
              << format_runtime_dump(
                     std::move(waits),
                     std::move(ready_tasks));
    std::cerr << std::flush;
}

} // namespace nxtrt::debug
