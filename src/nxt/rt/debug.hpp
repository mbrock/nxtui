#pragma once

#include "nxt/rt/ids.hpp"

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

namespace nxt::rt::debug {

using zone_id = std::uint64_t;

struct zone_snapshot
{
    zone_id id = 0;
    zone_id parent = 0;
    std::size_t children = 0;
    bool stopping = false;
};

struct wait_snapshot
{
    task_id task;
    std::uint64_t token = 0;
    std::chrono::steady_clock::time_point parked_at;
    std::string wish;
};

namespace detail {

inline std::atomic<zone_id> next_zone_id = 1;
inline std::mutex zones_mutex;
inline std::vector<zone_snapshot> zones;
inline std::mutex waits_mutex;
inline std::vector<wait_snapshot> waits;
inline volatile std::sig_atomic_t dump_requested = 0;

inline void signal_handler(int) noexcept
{
    dump_requested = 1;
}

} // namespace detail

[[nodiscard]] inline zone_id allocate_zone_id() noexcept
{
    return detail::next_zone_id.fetch_add(1, std::memory_order_relaxed);
}

inline void install_signal_dump(int signal = SIGUSR1)
{
    struct sigaction action {};
    action.sa_handler = detail::signal_handler;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(signal, &action, nullptr);
}

[[nodiscard]] inline bool consume_signal_dump_request() noexcept
{
    if (detail::dump_requested == 0)
        return false;
    detail::dump_requested = 0;
    return true;
}

inline void register_zone(zone_snapshot zone)
{
    auto lock = std::scoped_lock{detail::zones_mutex};
    detail::zones.push_back(zone);
}

inline void unregister_zone(zone_id id)
{
    auto lock = std::scoped_lock{detail::zones_mutex};
    std::erase_if(detail::zones, [id](const zone_snapshot & zone) {
        return zone.id == id;
    });
}

inline void update_zone(zone_snapshot zone)
{
    auto lock = std::scoped_lock{detail::zones_mutex};
    for (auto & existing : detail::zones) {
        if (existing.id == zone.id) {
            existing = zone;
            return;
        }
    }
}

[[nodiscard]] inline std::vector<zone_snapshot> snapshot_zones()
{
    auto lock = std::scoped_lock{detail::zones_mutex};
    return detail::zones;
}

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

inline void unpark_task(task_id task)
{
    auto lock = std::scoped_lock{detail::waits_mutex};
    std::erase_if(detail::waits, [task](const wait_snapshot & wait) {
        return wait.task == task;
    });
}

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

[[nodiscard]] inline std::string format_runtime_dump(
    std::vector<zone_snapshot> zones,
    std::vector<wait_snapshot> waits,
    std::vector<task_id> ready_tasks)
{
    auto now = std::chrono::steady_clock::now();
    auto out = std::ostringstream{};
    out << "[nxt::rt] runtime dump\n";
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

    out << "  zones: " << zones.size() << "\n";
    for (auto const & zone : zones) {
        out << "    zone " << zone.id;
        if (zone.parent != 0)
            out << " parent " << zone.parent;
        out << " children " << zone.children;
        if (zone.stopping)
            out << " stopping";
        out << "\n";
    }

    out << "  parked wishes: " << waits.size() << "\n";
    for (auto const & wait : waits) {
        out << "    task " << wait.task.value
            << " token " << wait.token
            << " parked " << format_duration(now - wait.parked_at)
            << "  " << wait.wish << "\n";
    }
    return out.str();
}

inline void print_runtime_dump(
    std::vector<zone_snapshot> zones,
    std::vector<wait_snapshot> waits,
    std::vector<task_id> ready_tasks)
{
    std::cerr << "\n"
              << format_runtime_dump(
                     std::move(zones),
                     std::move(waits),
                     std::move(ready_tasks));
    std::cerr << std::flush;
}

} // namespace nxt::rt::debug
