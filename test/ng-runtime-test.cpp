#include <nxt/rt/buffers.hpp>
#include <nxt/rt/pipe.hpp>
#include <nxt/rt/task.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::test {

using namespace boost::ut;
using namespace std::literals;

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

static suite ng_runtime_tests = [] {
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

    "rt buffer source visits chunks through reused storage"_test = [] {
        auto deck = nxt::rt::deck{};
        auto chunks = std::array{"ab"sv, "cdef"sv, "g"sv};
        auto source = nxt::rt::string_source{std::span{chunks}};
        auto storage = std::array<std::byte, 3>{};
        auto visited = std::vector<std::string>{};

        auto total = deck.sync_wait([&]() -> nxt::rt::task<std::size_t> {
            co_return co_await nxt::rt::for_each_chunk(
                source,
                std::span{storage},
                [&visited](std::span<const std::byte> chunk) {
                    visited.emplace_back(nxt::rt::as_string_view(chunk));
                });
        });

        expect(total == std::size_t{7});
        expect(visited == std::vector<std::string>{"abc", "def", "g"});
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

    "pipe yields values to an awaiting task"_test = [] {
        auto deck = nxt::rt::deck{};

        auto numbers = []() -> nxt::rt::pipe<int> {
            co_yield 1;
            co_yield 2;
            co_yield 3;
        };

        auto values = deck.sync_wait([&]() -> nxt::rt::task<std::vector<int>> {
            auto pipe = numbers();
            auto out = std::vector<int>{};
            while (auto value = co_await pipe.next())
                out.push_back(*value);
            co_return out;
        });

        expect(values == std::vector<int>{1, 2, 3});
    };

    "pipe can await between yielded values"_test = [] {
        auto deck = nxt::rt::deck{};

        auto paced_numbers = []() -> nxt::rt::pipe<int> {
            co_yield 1;
            co_await nxt::rt::yield();
            co_yield 2;
        };

        auto values = deck.sync_wait([&]() -> nxt::rt::task<std::vector<int>> {
            auto pipe = paced_numbers();
            auto out = std::vector<int>{};
            while (auto value = co_await pipe.next())
                out.push_back(*value);
            co_return out;
        });

        expect(values == std::vector<int>{1, 2});
    };

    "pipe can await child tasks before yielding values"_test = [] {
        auto deck = nxt::rt::deck{};

        auto child = [](int value) -> nxt::rt::task<int> {
            co_await nxt::rt::yield();
            co_return value * 2;
        };

        auto doubled = [child]() -> nxt::rt::pipe<int> {
            co_yield co_await child(2);
            co_yield co_await child(3);
        };

        auto values = deck.sync_wait([&]() -> nxt::rt::task<std::vector<int>> {
            auto pipe = doubled();
            auto out = std::vector<int>{};
            while (auto value = co_await pipe.next())
                out.push_back(*value);
            co_return out;
        });

        expect(values == std::vector<int>{4, 6});
    };

    "pipe can await a wand wish before yielding again"_test = [] {
        auto deck = nxt::rt::deck{};
        auto wand = manual_wand{};
        auto values = std::vector<int>{};

        auto consumer_body =
            [](std::vector<int> & values) -> nxt::rt::task<void> {
                auto producer = []() -> nxt::rt::pipe<int> {
                    co_yield 1;
                    co_await nxt::rt::manual_wish{.token = 99};
                    co_yield 2;
                };
                auto pipe = producer();
                while (auto value = co_await pipe.next())
                    values.push_back(*value);
            };

        auto consumer = consumer_body(values);

        deck.start(consumer);
        deck.run_until_idle(wand);

        expect(values == std::vector<int>{1});
        expect(wand.parked.size() == std::size_t{1});
        expect(wand.parked.front().token == std::uint64_t{99});

        wand.fulfill(deck, 99);
        deck.run_until_idle(wand);

        expect(values == std::vector<int>{1, 2});
        expect(consumer.done());
    };

    "pipe exceptions propagate to the awaiting task"_test = [] {
        auto deck = nxt::rt::deck{};

        auto broken = []() -> nxt::rt::pipe<int> {
            co_yield 1;
            throw std::runtime_error{"pipe boom"};
        };

        auto threw = false;
        try {
            deck.sync_wait([&]() -> nxt::rt::task<void> {
                auto pipe = broken();
                while (co_await pipe.next()) {
                }
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
