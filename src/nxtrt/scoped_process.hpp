#pragma once

#include "nxtrt/cgroup.hpp"
#include "nxtrt/subprocess.hpp"
#include "nxtrt/task.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nxtrt::scoped_process {

using namespace std::chrono_literals;

struct options
{
    bool systemd_user_scope = false;
    std::string unit_name = {};
    std::chrono::milliseconds sample_interval = 50ms;
    std::size_t max_samples = 128;
};

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

template<typename Tag>
[[nodiscard]] inline std::string make_unit_name(Tag tag)
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return std::format(
        "nxt-{}-{}-{:x}",
        tag,
        ::getpid(),
        static_cast<std::uint64_t>(nanos));
}

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
        std::format("--unit={}", std::move(unit_name)),
    };
    wrapped.reserve(wrapped.size() + argv.size());
    for (auto & arg : argv)
        wrapped.push_back(std::move(arg));
    return wrapped;
}

#if defined(__linux__)
struct piped_child
{
    subprocess::piped_child child;
    observation observed;
};

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
#endif

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
