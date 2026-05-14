#include <nxt/rt/task.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

struct manual_wand final : nxt::rt::wand
{
    nxt::rt::waiter<void> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::manual_wish wish) override
    {
        prepared.push_back(wish.token);
        auto state = std::make_shared<nxt::rt::wait_state<void>>();
        states.push_back(state);
        return nxt::rt::waiter<void>{*this, wish.token, state};
    }

    nxt::rt::waiter<int> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::openat_wish) override
    {
        throw std::runtime_error{"manual_wand does not implement openat"};
    }

    nxt::rt::waiter<std::size_t> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::read_some_wish) override
    {
        throw std::runtime_error{"manual_wand does not implement read"};
    }

    void suspend(nxt::rt::wait_token token, nxt::rt::parked_task task) override
    {
        parked.push_back(
            parked_entry{
                .token = token,
                .task = task,
            });
    }

    void wave(nxt::rt::deck &) override
    {
        ++waves;
    }

    void fulfill(nxt::rt::deck & deck, nxt::rt::wait_token token)
    {
        for (auto it = parked.begin(); it != parked.end(); ++it) {
            if (it->token != token)
                continue;

            auto task = it->task;
            states.front()->set_value();
            parked.erase(it);
            task.resume(deck);
            return;
        }
    }

    struct parked_entry
    {
        nxt::rt::wait_token token = 0;
        nxt::rt::parked_task task;
    };

    std::vector<nxt::rt::wait_token> prepared;
    std::vector<parked_entry> parked;
    std::vector<std::shared_ptr<nxt::rt::wait_state<void>>> states;
    int waves = 0;
};

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

    "manual wish prepares and parks a typed waiter"_test = [] {
        auto deck = nxt::rt::deck{};
        auto wand = manual_wand{};
        auto events = std::vector<int>{};

        auto task_body = [](std::vector<int> & events) -> nxt::rt::task<void> {
            events.push_back(1);
            co_await nxt::rt::manual_wish{.token = 42};
            events.push_back(2);
        };

        auto task = task_body(events);
        deck.start(task);
        deck.run_ready(wand);

        expect(events == std::vector<int>{1})
            << "task should suspend before manual wish fulfillment";
        expect(deck.empty()) << "manual wish should not requeue itself";
        expect(wand.prepared == std::vector<nxt::rt::wait_token>{42})
            << "wand should synchronously prepare the wish";
        expect(wand.parked.size() == std::size_t{1})
            << "waiter should park the suspended coroutine";
        expect(wand.parked.front().token == std::uint64_t{42});

        wand.fulfill(deck, 42);
        deck.run_ready(wand);

        expect(events == std::vector<int>{1, 2})
            << "fulfilled manual wish should resume the suspended task";
    };

    "run_ready waves the wand after staged wish preparation"_test = [] {
        auto deck = nxt::rt::deck{};
        auto wand = manual_wand{};
        auto events = std::vector<int>{};

        auto task_body = [](std::vector<int> & events) -> nxt::rt::task<void> {
            events.push_back(1);
            co_await nxt::rt::manual_wish{.token = 7};
            events.push_back(2);
        };

        auto task = task_body(events);
        deck.start(task);
        deck.run_ready(wand);

        expect(events == std::vector<int>{1});
        expect(wand.prepared == std::vector<nxt::rt::wait_token>{7});
        expect(wand.parked.size() == std::size_t{1});
        expect(wand.waves == 1_i)
            << "run_ready(wand) should wave after the pump round";

        wand.fulfill(deck, 7);
        deck.run_ready(wand);

        expect(events == std::vector<int>{1, 2});
        expect(wand.prepared.size() == std::size_t{1})
            << "resuming after fulfillment should not prepare a second wish";
        expect(wand.waves == 2_i);
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
