#include <nxt/rt/task.hpp>

#include <boost/ut.hpp>

#include <stdexcept>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

suite ng_runtime_tests = [] {
    "sync_wait returns a completed root task value"_test = [] {
        auto sched = nxt::rt::scheduler{};

        expect(sched.sync_wait([]() -> nxt::rt::task<int> {
            co_return 7;
        }) == 7_i);
    };

    "awaited child resumes its awaiting task"_test = [] {
        auto sched = nxt::rt::scheduler{};
        auto events = std::vector<int>{};

        auto child_body = [&events]() -> nxt::rt::task<int> {
            events.push_back(2);
            co_yield nxt::rt::yield;
            events.push_back(3);
            co_return 4;
        };

        expect(sched.sync_wait([&events, child_body]() -> nxt::rt::task<int> {
            events.push_back(1);
            auto child = child_body();
            auto child_id = child.id();

            auto value = co_await child;

            expect(child.id() == child_id);
            events.push_back(4);
            co_return value + 1;
        }) == 5_i);

        expect(events == std::vector<int>{1, 2, 3, 4});
    };

    "yield re-enters through the scheduler pump"_test = [] {
        auto sched = nxt::rt::scheduler{};
        auto out = std::vector<int>{};

        auto child_body =
            [&out](int tag) -> nxt::rt::task<void> {
            out.push_back(tag * 10 + 1);
            co_yield nxt::rt::yield;
            out.push_back(tag * 10 + 2);
        };

        sched.sync_wait([&]() -> nxt::rt::task<void> {
            auto first = child_body(1);
            auto second = child_body(2);

            co_await first;
            co_await second;
        });

        expect(out == std::vector<int>{11, 12, 21, 22});
    };

    "exceptions propagate through sync_wait"_test = [] {
        auto sched = nxt::rt::scheduler{};

        auto threw = false;
        try {
            sched.sync_wait([]() -> nxt::rt::task<void> {
                co_yield nxt::rt::yield;
                throw std::runtime_error{"boom"};
            });
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
