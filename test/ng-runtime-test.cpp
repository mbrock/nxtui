#include <nxt/rt/task.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

suite ng_runtime_tests = [] {
    "sync_wait returns a completed root task value"_test = [] {
        auto deck = nxt::rt::deck{};

        expect(deck.sync_wait([]() -> nxt::rt::task<int> {
            co_return 7;
        }) == 7_i);
    };

    "awaited child resumes its awaiting task"_test = [] {
        auto deck = nxt::rt::deck{};
        auto events = std::vector<int>{};

        auto child_body = [&events]() -> nxt::rt::task<int> {
            events.push_back(2);
            co_await nxt::rt::yield();
            events.push_back(3);
            co_return 4;
        };

        expect(deck.sync_wait([&events, child_body]() -> nxt::rt::task<int> {
            events.push_back(1);
            auto child = child_body();
            auto child_id = child.id();

            auto value = co_await child;

            expect(child.id() == child_id);
            events.push_back(4);
            co_return value + 1;
        }) == 5_i);

        expect(events == std::vector<int>{1, 2, 3, 4})
            << "child/continuation event order changed";
    };

    "yield re-enters through the deck pump"_test = [] {
        auto deck = nxt::rt::deck{};
        auto out = std::vector<int>{};

        auto child_body =
            [&out](int tag) -> nxt::rt::task<void> {
            out.push_back(tag * 10 + 1);
            co_await nxt::rt::yield();
            out.push_back(tag * 10 + 2);
        };

        deck.sync_wait([&]() -> nxt::rt::task<void> {
            auto first = child_body(1);
            auto second = child_body(2);

            co_await first;
            co_await second;
        });

        expect(out == std::vector<int>{11, 12, 21, 22})
            << "yield event order changed";
    };

    "run_ready only plays the initially ready round"_test = [] {
        auto deck = nxt::rt::deck{};
        auto events = std::vector<int>{};

        // Pass state as a coroutine parameter instead of capturing it in a
        // temporary coroutine lambda; captures live in the lambda object, while
        // parameters live in the coroutine frame.
        auto task_body = [](std::vector<int> & events) -> nxt::rt::task<void> {
            events.push_back(1);
            co_await nxt::rt::yield();
            events.push_back(2);
        };

        auto task = task_body(events);
        deck.start(task);
        deck.run_ready();

        expect(events == std::vector<int>{1})
            << "run_ready should only play the first ready round";
        expect(!deck.empty()) << "yielded task should be queued for next round";

        deck.run_ready();

        expect(events == std::vector<int>{1, 2})
            << "second run_ready should play the yielded task";
        expect(deck.empty()) << "deck should be empty after second round";
    };

    "run_until_idle keeps playing rounds until quiescence"_test = [] {
        auto deck = nxt::rt::deck{};
        auto events = std::vector<int>{};

        // Same lifetime rule as above: coroutine parameters are frame state.
        auto task_body = [](std::vector<int> & events) -> nxt::rt::task<void> {
            events.push_back(1);
            co_await nxt::rt::yield();
            events.push_back(2);
        };

        auto task = task_body(events);
        deck.start(task);
        deck.run_until_idle();

        expect(events == std::vector<int>{1, 2})
            << "run_until_idle should play all rounds";
        expect(deck.empty()) << "deck should be empty after run_until_idle";
    };

    "wish await records a resumable platform request"_test = [] {
        auto deck = nxt::rt::deck{};
        auto events = std::vector<int>{};

        auto task_body = [](std::vector<int> & events) -> nxt::rt::task<void> {
            events.push_back(1);
            co_await nxt::rt::wait_for(nxt::rt::wish::manual(42));
            events.push_back(2);
        };

        auto task = task_body(events);
        deck.start(task);
        deck.run_ready();

        expect(events == std::vector<int>{1})
            << "task should suspend before wish fulfillment";
        expect(deck.empty()) << "wish await should not requeue itself";

        auto wishes = deck.take_wishes();
        expect(wishes.size() == std::size_t{1})
            << "one wish should be collected";
        expect(wishes.front().desired.what == nxt::rt::wish::kind::manual);
        expect(wishes.front().desired.token == std::uint64_t{42});

        wishes.front().fulfill(deck);
        deck.run_ready();

        expect(events == std::vector<int>{1, 2})
            << "fulfilled wish should resume the suspended task";
    };

    "run_ready can post collected wishes to a wand"_test = [] {
        struct recording_wand final : nxt::rt::wand
        {
            void post(nxt::rt::deck &, nxt::rt::wish_request request) override
            {
                posted.push_back(request);
            }

            std::vector<nxt::rt::wish_request> posted;
        };

        auto deck = nxt::rt::deck{};
        auto wand = recording_wand{};
        auto events = std::vector<int>{};

        auto task_body = [](std::vector<int> & events) -> nxt::rt::task<void> {
            events.push_back(1);
            co_await nxt::rt::wait_for(nxt::rt::wish::manual(7));
            events.push_back(2);
        };

        auto task = task_body(events);
        deck.start(task);
        deck.run_ready(wand);

        expect(events == std::vector<int>{1});
        expect(wand.posted.size() == std::size_t{1})
            << "run_ready(wand) should post collected wishes";
        expect(wand.posted.front().desired.token == std::uint64_t{7});

        wand.posted.front().fulfill(deck);
        deck.run_ready(wand);

        expect(events == std::vector<int>{1, 2});
        expect(wand.posted.size() == std::size_t{1})
            << "resuming after fulfillment should not post a second wish";
    };

    "exceptions propagate through sync_wait"_test = [] {
        auto deck = nxt::rt::deck{};

        auto threw = false;
        try {
            deck.sync_wait([]() -> nxt::rt::task<void> {
                co_await nxt::rt::yield();
                throw std::runtime_error{"boom"};
            });
        } catch (const std::runtime_error &) {
            threw = true;
        }
        expect(threw);
    };

    "deck pump rejects reentrant calls from a running task"_test = [] {
        auto deck = nxt::rt::deck{};

        expect(deck.sync_wait([&deck]() -> nxt::rt::task<bool> {
            try {
                deck.run_ready();
            } catch (const std::runtime_error &) {
                co_return true;
            }
            co_return false;
        }));
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
