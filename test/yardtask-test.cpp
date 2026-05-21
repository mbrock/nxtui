#include <nxtio/async.hpp>
#include <nxtio/yardtask.hpp>

#include <boost/ut.hpp>

#include <chrono>

namespace nxt::test {

using namespace boost::ut;
using namespace std::chrono_literals;

nxt::yard_var<int> ambient_value{"ambient-value"};

template<typename T>
nxt::task<T> drive_yardtask(nxt::yardtask<T> task)
{
    co_return co_await std::move(task);
}

nxt::task<> drive_yardtask(nxt::yardtask<> task)
{
    co_await std::move(task);
}

nxt::yardtask<int> child_reads_inherited_context()
{
    auto frame = nxt::yard_current_frame();
    auto has_parent = frame && !frame->parent.expired();
    co_return ambient_value.get() + (has_parent ? 1 : 0);
}

nxt::yardtask<int> parent_awaits_child()
{
    ambient_value.set(10);
    co_return co_await child_reads_inherited_context();
}

nxt::yardtask<int>
context_survives_libcoro_await(nxt::scheduler & scheduler)
{
    ambient_value.set(41);
    co_await scheduler.yield_for(1ms);
    co_return ambient_value.get();
}

nxt::yardtask<int> spawned_child(nxt::scheduler & scheduler)
{
    co_await scheduler.yield_for(1ms);
    auto frame = nxt::yard_current_frame();
    auto has_parent = frame && !frame->parent.expired();
    co_return ambient_value.get() + (has_parent ? 1 : 0);
}

nxt::yardtask<int> parent_spawns_child(nxt::scheduler & scheduler)
{
    ambient_value.set(20);
    auto parent_frame = nxt::yard_current_frame();
    auto child = nxt::yard_spawn(scheduler, spawned_child(scheduler));
    auto has_structured_child =
        parent_frame && !parent_frame->children.empty();

    auto result = co_await child.join();
    co_return result + (has_structured_child ? 100 : 0);
}

static suite yardtask_tests{
    "Yard tasks", [] {
        "child tasks"_test = [] {
            "inherit ambient context and parent relations when awaited"_test =
                [] {
                    expect(nxt::sync_wait(parent_awaits_child()) == 11_i);
                };

            "preserve ambient yard frames through libcoro awaits"_test =
                [] {
                    auto scheduler = nxt::scheduler::make_unique();
                    auto result =
                        nxt::sync_wait(scheduler->schedule(drive_yardtask(
                            context_survives_libcoro_await(*scheduler))));

                    expect(result == 41_i);
                };

            "are structured and joinable when spawned"_test = [] {
                auto scheduler = nxt::scheduler::make_unique();
                auto result = nxt::sync_wait(scheduler->schedule(
                    drive_yardtask(parent_spawns_child(*scheduler))));

                expect(result == 121_i);
            };
        };
    }};

} // namespace nxt::test
