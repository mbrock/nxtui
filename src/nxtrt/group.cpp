#include "nxtrt/task.hpp"

namespace nxtrt::detail {

task<void>
run_group(std::span<group_recipe> recipes, group_control & control)
{
    // A group whose task is already stopped starts nothing.
    if (task_stop_requested())
        throw operation_cancelled{};
    if (recipes.empty())
        co_return;
    auto land = pool_land<group_recipe>{recipes.size()};
    auto input_land = static_value_storage<group_recipe, 1>{};
    auto input = value_range_source{recipes, input_land.ref()};
    auto jobs = pool<group_recipe>{input, land.slots(), land.output()};
    control.jobs = &jobs;
    control.stop_jobs = [](void * jobs) noexcept {
        static_cast<pool<group_recipe> *>(jobs)->stop();
    };
    try {
        co_await finally(discard_results(jobs), [&jobs] {
            return jobs.close();
        });
    } catch (const operation_cancelled &) {
        if (!control.stopped || task_stop_requested())
            throw;
    }
    control.stop_jobs = nullptr;
    if (task_stop_requested())
        throw operation_cancelled{};
}

} // namespace nxtrt::detail
