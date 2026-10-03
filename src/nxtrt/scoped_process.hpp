#pragma once

#include "nxtrt/cgroup.hpp"
#include "nxtrt/subprocess.hpp"
#include "nxtrt/task.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include "nxtrt/format.hpp"
#include <optional>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

/**
 * @namespace nxtrt::scoped_process
 * Child processes optionally run in a systemd user scope, with cgroup
 * resource sampling.
 *
 * `spawn_piped` is `nxtrt::subprocess::spawn_piped` that can wrap the
 * command in `systemd-run --user --scope`, which puts the child and all its
 * descendants in their own cgroup. `monitor_until_done` then samples that
 * cgroup's memory, CPU, pid count and pressure (see `nxtrt::cgroup`) while
 * the caller waits for the child. Without a scope it spawns directly and
 * monitoring does nothing. Scopes need Linux with a systemd user manager.
 * Nothing here uses the scope to stop descendants.
 */
namespace nxtrt::scoped_process {

using namespace std::chrono_literals;

/// How `spawn_piped` runs the child and how a caller should monitor it.
struct options
{
    /// Wrap the command in `systemd-run --user --scope`.
    bool systemd_user_scope = false;
    /// Scope unit name (without ".scope"); empty picks a unique one.
    std::string unit_name = {};
    /// For the caller to pass to `monitor_until_done`; `spawn_piped`
    /// ignores it.
    std::chrono::milliseconds sample_interval = 50ms;
    /// For the caller to pass to `monitor_until_done`; `spawn_piped`
    /// ignores it.
    std::size_t max_samples = 128;
};

/// The scope a child runs in and the cgroup samples taken from it.
/// Inactive (empty unit name) when the child was not spawned in a scope.
struct observation
{
    std::string unit_name = {};
    std::filesystem::path cgroup_path = {};
    std::vector<cgroup::sample> samples = {};

    [[nodiscard]] bool active() const noexcept
    {
        return !unit_name.empty();
    }

    [[nodiscard]] std::optional<cgroup::sample> latest() const
    {
        if (samples.empty())
            return std::nullopt;
        return samples.back();
    }
};

/// A unit name "nxt-TAG-PID-NANOS" that is unique per call in practice.
template<typename Tag>
[[nodiscard]] inline std::string make_unit_name(Tag tag)
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return nxtrt::format(
        "nxt-{}-{}-{:x}",
        tag,
        ::getpid(),
        static_cast<std::uint64_t>(nanos));
}

/// ARGV prefixed with `systemd-run --user --scope --quiet --collect
/// --unit=UNIT_NAME`.
[[nodiscard]] inline std::vector<std::string> systemd_scope_argv(
    std::string unit_name,
    std::vector<std::string> argv)
{
    auto wrapped = std::vector<std::string>{
        "systemd-run",
        "--user",
        "--scope",
        "--quiet",
        "--collect",
        nxtrt::format("--unit={}", unit_name),
    };
    wrapped.reserve(wrapped.size() + argv.size());
    for (auto & arg : argv)
        wrapped.push_back(std::move(arg));
    return wrapped;
}

/// A `subprocess::piped_child` and its scope observation. With a scope,
/// the child process is `systemd-run`, which execs the command.
struct piped_child
{
    subprocess::piped_child child;
    observation observed;
};

/// `subprocess::spawn_piped(ARGV)`, wrapped in a systemd user scope when
/// OPTS asks for one. The scope's cgroup path is not known yet; the
/// observation finds it on the first `monitor_until_done` sample.
inline task<piped_child> spawn_piped(
    std::vector<std::string> argv,
    options opts = {})
{
    auto observed = observation{};
    if (opts.systemd_user_scope) {
        observed.unit_name = opts.unit_name.empty()
            ? make_unit_name("tool")
            : std::move(opts.unit_name);
        argv = systemd_scope_argv(observed.unit_name, std::move(argv));
    }

    co_return piped_child{
        .child = co_await subprocess::spawn_piped(std::move(argv)),
        .observed = std::move(observed),
    };
}

/// Samples OBSERVED's cgroup every INTERVAL until DONE becomes true or the
/// task is stopped, keeping the last MAX_SAMPLES samples.
///
/// Returns at once for an inactive observation. Until the scope's cgroup
/// is found, each round searches the whole cgroup tree under
/// /sys/fs/cgroup. If DONE ended the loop (not a stop), it takes one final
/// sample. OBSERVED and DONE are borrowed and must outlive the task; run
/// it beside the task that waits for the child and sets DONE. Cancellation
/// ends it normally rather than throwing.
inline task<void> monitor_until_done(
    observation & observed,
    const bool & done,
    std::chrono::milliseconds interval = 50ms,
    std::size_t max_samples = 128)
{
    if (!observed.active())
        co_return;

    try {
        while (!done && !stop_requested()) {
            if (observed.cgroup_path.empty()) {
                if (auto found =
                        co_await cgroup::find_unit_scope(observed.unit_name))
                    observed.cgroup_path = std::move(*found);
            }

            if (!observed.cgroup_path.empty()) {
                observed.samples.push_back(
                    co_await cgroup::read_sample(observed.cgroup_path));
                if (observed.samples.size() > max_samples)
                    observed.samples.erase(
                        observed.samples.begin(),
                        observed.samples.begin()
                            + static_cast<std::ptrdiff_t>(
                                observed.samples.size() - max_samples));
            }

            co_await op::timeout::after(interval);
        }
    } catch (const operation_cancelled &) {
    }

    if (done && !stop_requested() && !observed.cgroup_path.empty())
        observed.samples.push_back(
            co_await cgroup::read_sample(observed.cgroup_path));
}

} // namespace nxtrt::scoped_process
