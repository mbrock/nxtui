#include <nxt/rt/task.hpp>

#include <boost/ut.hpp>

#include <stdexcept>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

suite ng_runtime_tests = [] {
    "sync_wait returns a completed root task value"_test = [] {
        auto sched = nxt::rt::scheduler{};

        auto root = []() -> nxt::rt::task<int> {
            co_return 7;
        };

        expect(sched.sync_wait(root()) == 7_i);
    };

    "awaited child is adopted by the current task"_test = [] {
        auto sched = nxt::rt::scheduler{};

        auto child_body = [&sched]() -> nxt::rt::task<int> {
            co_yield nxt::rt::yield;
            co_return 4;
        };

        auto parent_body = [&sched, child_body]() -> nxt::rt::task<int> {
            auto parent_id = sched.current_task_id();
            auto child = child_body();
            auto child_id = child.id();

            auto value = co_await child;

            expect(child.id() == child_id);
            expect(child.parent_id().has_value());
            expect(*child.parent_id() == parent_id);
            co_return value + 1;
        };

        expect(sched.sync_wait(parent_body()) == 5_i);
    };

    "yield re-enters through the scheduler pump"_test = [] {
        auto sched = nxt::rt::scheduler{};
        auto out = std::vector<int>{};

        auto child_body =
            [&sched, &out](int tag) -> nxt::rt::task<void> {
            out.push_back(tag * 10 + 1);
            co_yield nxt::rt::yield;
            out.push_back(tag * 10 + 2);
        };

        auto parent_body = [&]() -> nxt::rt::task<void> {
            auto first = child_body(1);
            auto second = child_body(2);

            co_await first;
            co_await second;
        };

        sched.sync_wait(parent_body());

        expect(out == std::vector<int>{11, 12, 21, 22});
    };

    "exceptions propagate through sync_wait"_test = [] {
        auto sched = nxt::rt::scheduler{};
        auto root = [&sched]() -> nxt::rt::task<void> {
            co_yield nxt::rt::yield;
            throw std::runtime_error{"boom"};
        };

        auto threw = false;
        try {
            sched.sync_wait(root());
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
