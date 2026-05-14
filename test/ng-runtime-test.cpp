#include <nxt/rt/task.hpp>

#include <boost/ut.hpp>

#include <stdexcept>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

nxt::rt::task<int> immediate_value()
{
    co_return 7;
}

nxt::rt::task<int> child_reads_parent(nxt::rt::scheduler & sched)
{
    co_await sched.yield();
    co_return 4;
}

nxt::rt::task<int> parent_awaits_child(nxt::rt::scheduler & sched)
{
    auto parent_id = sched.current_task_id();
    auto child = child_reads_parent(sched);
    auto child_id = child.id();
    auto value = co_await child;
    expect(child.id() == child_id);
    expect(child.parent_id().has_value());
    expect(*child.parent_id() == parent_id);
    co_return value + 1;
}

nxt::rt::task<void> yield_sequence(
    nxt::rt::scheduler & sched,
    std::vector<int> & out,
    int tag)
{
    out.push_back(tag * 10 + 1);
    co_await sched.yield();
    out.push_back(tag * 10 + 2);
}

nxt::rt::task<void> parent_runs_two_children(
    nxt::rt::scheduler & sched,
    std::vector<int> & out)
{
    auto a = yield_sequence(sched, out, 1);
    auto b = yield_sequence(sched, out, 2);
    co_await a;
    co_await b;
}

nxt::rt::task<void> throws_after_yield(nxt::rt::scheduler & sched)
{
    co_await sched.yield();
    throw std::runtime_error{"boom"};
}

suite ng_runtime_tests = [] {
    "sync_wait returns a completed root task value"_test = [] {
        auto sched = nxt::rt::scheduler{};
        expect(sched.sync_wait(immediate_value()) == 7_i);
    };

    "awaited child is adopted by the current task"_test = [] {
        auto sched = nxt::rt::scheduler{};
        expect(sched.sync_wait(parent_awaits_child(sched)) == 5_i);
    };

    "yield re-enters through the scheduler pump"_test = [] {
        auto sched = nxt::rt::scheduler{};
        auto out = std::vector<int>{};
        sched.sync_wait(parent_runs_two_children(sched, out));
        expect(out == std::vector<int>{11, 12, 21, 22});
    };

    "exceptions propagate through sync_wait"_test = [] {
        auto sched = nxt::rt::scheduler{};
        auto threw = false;
        try {
            sched.sync_wait(throws_after_yield(sched));
        } catch (const std::runtime_error &) {
            threw = true;
        }
        expect(threw);
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
