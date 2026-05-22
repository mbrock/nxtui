#include <nxt/rt/buffers.hpp>
#include <nxt/rt/http.hpp>
#include <nxt/rt/pipe.hpp>
#include <nxt/rt/task.hpp>

#include "test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace nxt::test {

using namespace boost::ut;
using namespace std::literals;

struct ambient_int_key
{
    using value_type = int;
    static constexpr auto name = "ambient-int";
};

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

    nxt::rt::waiter<std::size_t> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::recv_some_wish) override
    {
        throw std::runtime_error{"manual_wand does not implement recv"};
    }

    nxt::rt::waiter<std::size_t> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::send_some_wish) override
    {
        throw std::runtime_error{"manual_wand does not implement send"};
    }

    nxt::rt::waiter<void> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::connect_wish) override
    {
        throw std::runtime_error{"manual_wand does not implement connect"};
    }

    nxt::rt::waiter<int> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::poll_wish) override
    {
        throw std::runtime_error{"manual_wand does not implement poll"};
    }

    nxt::rt::waiter<void> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::timeout_wish) override
    {
        throw std::runtime_error{"manual_wand does not implement timeout"};
    }

    nxt::rt::waiter<nxt::rt::poll_until_result> prepare(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::poll_until_wish) override
    {
        throw std::runtime_error{
            "manual_wand does not implement poll-until"};
    }

    void
    suspend(nxt::rt::wait_token token, nxt::rt::parked_task task) override
    {
        parked.push_back(
            parked_entry{
                .token = token,
                .task = task,
            });
    }

    void cancel(nxt::rt::wait_token token) override
    {
        cancelled.push_back(token);
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
    std::vector<nxt::rt::wait_token> cancelled;
    std::vector<parked_entry> parked;
    std::vector<std::shared_ptr<nxt::rt::wait_state<void>>> states;
    int waves = 0;
};

struct empty_then_string_source final : nxt::rt::byte_source
{
    nxt::rt::task<nxt::rt::read_result>
    read_some(std::span<std::byte> dst) override
    {
        if (!returned_empty) {
            returned_empty = true;
            co_return nxt::rt::read_result{
                .bytes = 0,
                .eof = false,
            };
        }

        if (offset == text.size())
            co_return nxt::rt::read_result{
                .bytes = 0,
                .eof = true,
            };

        auto rest = std::string_view{text}.substr(offset);
        auto n = std::min(dst.size(), rest.size());
        std::memcpy(dst.data(), rest.data(), n);
        offset += n;
        co_return nxt::rt::read_result{
            .bytes = n,
            .eof = offset == text.size(),
        };
    }

    std::string_view text = "abc";
    std::size_t offset = 0;
    bool returned_empty = false;
};

nxt::rt::task<int> read_ambient_int_after_yield()
{
    co_await nxt::rt::yield();
    co_return nxt::rt::env_require<ambient_int_key>();
}

nxt::rt::task<int> read_ambient_int()
{
    co_return nxt::rt::env_require<ambient_int_key>();
}

nxt::rt::task<void> record_after_yield(std::vector<int> & events, int value)
{
    events.push_back(value * 10 + 1);
    co_await nxt::rt::yield();
    events.push_back(value * 10 + 2);
}

nxt::rt::task<void> record_current_zone(
    std::vector<nxt::rt::task_zone *> & zones)
{
    co_await nxt::rt::yield();
    zones.push_back(nxt::rt::current_zone());
}

nxt::rt::task<void> throw_after_yield(std::vector<int> & events, int value)
{
    events.push_back(value * 10 + 1);
    co_await nxt::rt::yield();
    throw nxt::rt::runtime_error{"zone child boom"};
}

nxt::rt::task<int> value_after_yield(int value)
{
    co_await nxt::rt::yield();
    co_return value;
}

nxt::rt::task<std::string> string_after_yield(std::string value)
{
    co_await nxt::rt::yield();
    co_return value;
}

nxt::rt::task<int> value_after_two_yields_or_stop(
    std::vector<int> & events,
    int value)
{
    co_await nxt::rt::yield();
    co_await nxt::rt::yield();
    if (nxt::rt::stop_requested()) {
        events.push_back(value);
        throw nxt::rt::operation_cancelled{};
    }
    co_return -value;
}

nxt::rt::task<int> throw_int_after_yield()
{
    co_await nxt::rt::yield();
    throw nxt::rt::runtime_error{"zone child int boom"};
}

nxt::rt::task<void> record_stop_state_after_yield(
    std::vector<int> & events,
    int value)
{
    co_await nxt::rt::yield();
    events.push_back(nxt::rt::stop_requested() ? value : -value);
}

nxt::rt::task<void> record_stop_state_after_two_yields(
    std::vector<int> & events,
    int value)
{
    co_await nxt::rt::yield();
    co_await nxt::rt::yield();
    events.push_back(nxt::rt::stop_requested() ? value : -value);
}

nxt::rt::task<void> record_task_stop_state_after_yield(
    std::vector<int> & events,
    int value)
{
    co_await nxt::rt::yield();
    events.push_back(nxt::rt::task_stop_requested() ? value : -value);
}

static suite ng_runtime_tests{
    "Runtime", [] {
        "deck"_test = [] {
            "sync_wait returns completed root task values"_test = [] {
                auto deck = nxt::rt::deck{};

                expect(deck.sync_wait([]() -> nxt::rt::task<int> {
                    co_return 7;
                }) == 7_i);
            };

            "resumes tasks awaiting children"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                auto child_body = [&events]() -> nxt::rt::task<int> {
                    events.push_back(2);
                    co_await nxt::rt::yield();
                    events.push_back(3);
                    co_return 4;
                };

                expect(
                    deck.sync_wait(
                        [&events, child_body]() -> nxt::rt::task<int> {
                            events.push_back(1);
                            auto child = child_body();
                            auto child_id = child.id();

                            auto value = co_await child;

                            expect(child.id() == child_id);
                            events.push_back(4);
                            co_return value + 1;
                        })
                    == 5_i);

                expect(events == std::vector<int>{1, 2, 3, 4})
                    << "child/continuation event order changed";
            };

            "re-enters yielded tasks through the pump"_test = [] {
                auto deck = nxt::rt::deck{};
                auto out = std::vector<int>{};

                auto child_body = [&out](int tag) -> nxt::rt::task<void> {
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

                // Pass state as a coroutine parameter instead of capturing
                // it in a temporary coroutine lambda; captures live in the
                // lambda object, while parameters live in the coroutine
                // frame.
                auto task_body =
                    [](std::vector<int> & events) -> nxt::rt::task<void> {
                    events.push_back(1);
                    co_await nxt::rt::yield();
                    events.push_back(2);
                };

                auto task = task_body(events);
                deck.start(task);
                deck.run_ready();

                expect(events == std::vector<int>{1})
                    << "run_ready should only play the first ready round";
                expect(!deck.empty())
                    << "yielded task should be queued for next round";

                deck.run_ready();

                expect(events == std::vector<int>{1, 2})
                    << "second run_ready should play the yielded task";
                expect(deck.empty())
                    << "deck should be empty after second round";
            };

            "run_until_idle plays rounds until quiescence"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                // Same lifetime rule as above: coroutine parameters are
                // frame state.
                auto task_body =
                    [](std::vector<int> & events) -> nxt::rt::task<void> {
                    events.push_back(1);
                    co_await nxt::rt::yield();
                    events.push_back(2);
                };

                auto task = task_body(events);
                deck.start(task);
                deck.run_until_idle();

                expect(events == std::vector<int>{1, 2})
                    << "run_until_idle should play all rounds";
                expect(deck.empty())
                    << "deck should be empty after run_until_idle";
            };

            "propagates exceptions through sync_wait"_test = [] {
                auto deck = nxt::rt::deck{};

                auto threw = false;
                try {
                    deck.sync_wait([]() -> nxt::rt::task<void> {
                        co_await nxt::rt::yield();
                        throw std::runtime_error{"boom"};
                    });
                } catch (const std::exception &) {
                    threw = true;
                }
                expect(threw);
            };

            "rejects reentrant pump calls"_test = [] {
                auto deck = nxt::rt::deck{};

                expect(deck.sync_wait([&deck]() -> nxt::rt::task<bool> {
                    try {
                        deck.run_ready();
                    } catch (const std::exception &) {
                        co_return true;
                    }
                    co_return false;
                }));
            };

            "tasks observe their own stop request"_test = [] {
                auto deck = nxt::rt::deck{};

                auto task = []() -> nxt::rt::task<bool> {
                    co_return nxt::rt::task_stop_requested();
                }();
                task.request_stop();

                expect(deck.sync_wait(std::move(task)));
            };
        };

        "environment"_test = [] {
            auto deck = nxt::rt::deck{};

            "survives nested task awaits"_test = [&] {
                auto result = deck.sync_wait([]() -> nxt::rt::task<int> {
                    co_return co_await nxt::rt::with_env<ambient_int_key>(
                        41, [] { return read_ambient_int_after_yield(); });
                });

                expect(result == 41_i);
            };

            "restores outer bindings"_test = [&] {
                auto result = deck.sync_wait([]() -> nxt::rt::task<int> {
                    co_return co_await nxt::rt::with_env<ambient_int_key>(
                        10, []() -> nxt::rt::task<int> {
                            auto before = co_await read_ambient_int();
                            auto inside = co_await nxt::rt::with_env<
                                ambient_int_key>(20, [] {
                                return read_ambient_int_after_yield();
                            });
                            auto after = co_await read_ambient_int();
                            co_return before * 100 + inside * 10 + after;
                        });
                });

                expect(result == 1210_i);
            };
        };

        "zones"_test = [] {
            "bind the current zone while the body runs"_test = [] {
                auto deck = nxt::rt::deck{};

                auto seen = deck.sync_wait([]() -> nxt::rt::task<bool> {
                    co_return co_await nxt::rt::with_zone(
                        []() -> nxt::rt::task<bool> {
                            auto * before = nxt::rt::current_zone();
                            co_await nxt::rt::yield();
                            auto * after = nxt::rt::current_zone();
                            co_return before != nullptr && before == after;
                        });
                });

                expect(seen);
            };

            "join forked tasks before the zone exits"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        nxt::rt::fork(record_after_yield(events, 1));
                        events.push_back(2);
                        co_return;
                    });
                    events.push_back(3);
                    co_return;
                });

                expect(events == std::vector<int>{2, 11, 12, 3});
            };

            "let forked tasks inherit the current zone"_test = [] {
                auto deck = nxt::rt::deck{};
                auto zones = std::vector<nxt::rt::task_zone *>{};
                auto expected = static_cast<nxt::rt::task_zone *>(nullptr);

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        expected = nxt::rt::current_zone();
                        nxt::rt::fork(record_current_zone(zones));
                        co_return;
                    });
                    co_return;
                });

                expect(expected != nullptr);
                expect(zones == std::vector<nxt::rt::task_zone *>{expected});
            };

            "allow children to fork more work into the same zone"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                auto parent = [&events]() -> nxt::rt::task<void> {
                    events.push_back(1);
                    co_await nxt::rt::yield();
                    nxt::rt::fork(record_after_yield(events, 2));
                    events.push_back(3);
                    co_return;
                };

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        nxt::rt::fork(parent());
                        co_return;
                    });
                    events.push_back(4);
                    co_return;
                });

                expect(events == std::vector<int>{1, 3, 21, 22, 4});
            };

            "reject fork outside a zone"_test = [] {
                auto deck = nxt::rt::deck{};

                auto rejected = deck.sync_wait([]() -> nxt::rt::task<bool> {
                    try {
                        nxt::rt::fork([]() -> nxt::rt::task<void> {
                            co_return;
                        }());
                    } catch (const std::exception &) {
                        co_return true;
                    }
                    co_return false;
                });

                expect(rejected);
            };

            "propagate child exceptions after joining siblings"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    deck.sync_wait([&]() -> nxt::rt::task<void> {
                        co_await nxt::rt::with_zone(
                            [&]() -> nxt::rt::task<void> {
                                nxt::rt::fork(throw_after_yield(events, 0));
                                nxt::rt::fork(record_after_yield(events, 2));
                                co_return;
                            });
                        co_return;
                    });
                } catch (const std::exception &) {
                    threw = true;
                }

                expect(threw);
                expect(events == std::vector<int>{1, 21, 22});
            };

            "group multiple child exceptions"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};
                auto grouped = false;

                try {
                    deck.sync_wait([&]() -> nxt::rt::task<void> {
                        co_await nxt::rt::with_zone(
                            [&]() -> nxt::rt::task<void> {
                                nxt::rt::fork(throw_after_yield(events, 1));
                                nxt::rt::fork(throw_after_yield(events, 2));
                                nxt::rt::fork(record_after_yield(events, 3));
                                co_return;
                            });
                        co_return;
                    });
                } catch (const nxt::rt::exception_group & group) {
                    grouped = true;
                    expect(group.exceptions().size() == std::size_t{2});
                }

                expect(grouped);
                expect(events == std::vector<int>{11, 21, 31, 32});
            };

            "return forked task results after joining"_test = [] {
                auto deck = nxt::rt::deck{};

                auto child =
                    deck.sync_wait([]()
                        -> nxt::rt::task<nxt::rt::deed<int>> {
                        co_return co_await nxt::rt::with_zone(
                            []() -> nxt::rt::task<nxt::rt::deed<int>> {
                                auto child =
                                    nxt::rt::fork(value_after_yield(42));
                                co_return std::move(child);
                            });
                    });

                expect(std::move(child).get() == 42_i);
            };

            "return several forked task results"_test = [] {
                auto deck = nxt::rt::deck{};
                using children_type = std::tuple<
                    nxt::rt::deed<int>,
                    nxt::rt::deed<int>>;

                auto children =
                    deck.sync_wait([]() -> nxt::rt::task<children_type> {
                        co_return co_await nxt::rt::with_zone(
                            []() -> nxt::rt::task<children_type> {
                                auto first =
                                    nxt::rt::fork(value_after_yield(10));
                                auto second =
                                    nxt::rt::fork(value_after_yield(20));
                                co_return children_type{
                                    std::move(first),
                                    std::move(second)};
                            });
                    });

                auto [first, second] = std::move(children);
                expect(std::move(first).get() == 10_i);
                expect(std::move(second).get() == 20_i);
            };

            "return failed deeds for caller observation"_test = [] {
                auto deck = nxt::rt::deck{};

                auto child =
                    deck.sync_wait([]()
                        -> nxt::rt::task<nxt::rt::deed<int>> {
                        co_return co_await nxt::rt::with_zone(
                            []() -> nxt::rt::task<nxt::rt::deed<int>> {
                                auto child =
                                    nxt::rt::fork(throw_int_after_yield());
                                co_return std::move(child);
                            });
                    });

                auto threw = false;
                try {
                    (void)std::move(child).get();
                } catch (const std::exception &) {
                    threw = true;
                }

                expect(threw);
            };

            "allow observed deed failures inside the zone"_test = [] {
                auto deck = nxt::rt::deck{};
                auto observed = false;

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        auto child =
                            nxt::rt::fork(throw_int_after_yield());
                        co_await nxt::rt::yield();
                        co_await nxt::rt::yield();
                        observed = child.exception() != nullptr;
                        co_return;
                    });
                });

                expect(observed);
            };

            "let coped deeds report failure as expected"_test = [] {
                auto deck = nxt::rt::deck{};

                auto child =
                    deck.sync_wait([]()
                        -> nxt::rt::task<nxt::rt::catching_deed<int>> {
                        co_return co_await nxt::rt::with_zone(
                            []()
                                -> nxt::rt::task<
                                    nxt::rt::catching_deed<int>> {
                                auto child =
                                    nxt::rt::fork(throw_int_after_yield())
                                        .cope();
                                co_return std::move(child);
                            });
                    });

                auto result = std::move(child).get();
                expect(!result.has_value());
            };

            "let coped deeds report success as expected"_test = [] {
                auto deck = nxt::rt::deck{};

                auto child =
                    deck.sync_wait([]()
                        -> nxt::rt::task<nxt::rt::catching_deed<int>> {
                        co_return co_await nxt::rt::with_zone(
                            []()
                                -> nxt::rt::task<
                                    nxt::rt::catching_deed<int>> {
                                auto child =
                                    nxt::rt::fork(value_after_yield(99))
                                        .cope();
                                co_return std::move(child);
                            });
                    });

                auto result = std::move(child).get();
                expect(result.has_value());
                expect(*result == 99_i);
            };

            "share a stop token with forked children"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        nxt::rt::fork(
                            record_stop_state_after_yield(events, 1));
                        nxt::rt::require_current_zone().stop();
                        co_return;
                    });
                });

                expect(events == std::vector<int>{1});
            };

            "reject fork after zone stop"_test = [] {
                auto deck = nxt::rt::deck{};
                auto rejected = false;

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        nxt::rt::require_current_zone().stop();
                        try {
                            nxt::rt::fork(value_after_yield(1));
                        } catch (const std::exception &) {
                            rejected = true;
                        }
                        co_return;
                    });
                });

                expect(rejected);
            };

            "request child stop when the zone body fails"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    deck.sync_wait([&]() -> nxt::rt::task<void> {
                        co_await nxt::rt::with_zone(
                            [&]() -> nxt::rt::task<void> {
                                nxt::rt::fork(
                                    record_stop_state_after_yield(events, 2));
                                throw nxt::rt::runtime_error{
                                    "zone body boom"};
                            });
                    });
                } catch (const std::exception &) {
                    threw = true;
                }

                expect(threw);
                expect(events == std::vector<int>{2});
            };

            "request task stop on forked children when the zone stops"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        nxt::rt::fork(
                            record_task_stop_state_after_yield(events, 3));
                        nxt::rt::require_current_zone().stop();
                        co_return;
                    });
                });

                expect(events == std::vector<int>{3});
            };

            "stop a hosted zone when its parent task is stopped"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                auto task = [&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        nxt::rt::fork(
                            record_stop_state_after_yield(events, 4));
                        events.push_back(100);
                        co_await nxt::rt::yield();
                        co_return;
                    });
                }();

                deck.start(task);
                for (auto i = 0; i != 8 && events.empty(); ++i)
                    deck.run_ready();

                expect(events == std::vector<int>{100});
                task.request_stop();
                deck.run_until_idle();

                expect(events == std::vector<int>{100, 4});
            };

            "stop-on-failure fork policy stops siblings"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    deck.sync_wait([&]() -> nxt::rt::task<void> {
                        co_await nxt::rt::with_zone(
                            nxt::rt::stop_on_failure{},
                            [&](auto & policy) -> nxt::rt::task<void> {
                                policy.fork(throw_after_yield(events, 1));
                                policy.fork(
                                    record_stop_state_after_two_yields(
                                        events,
                                        2));
                                co_return;
                            });
                    });
                } catch (const std::exception &) {
                    threw = true;
                }

                expect(threw);
                expect(events == std::vector<int>{11, 2});
            };

            "stop-on-success fork policy stops siblings"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                auto child =
                    deck.sync_wait([&]()
                        -> nxt::rt::task<nxt::rt::deed<int>> {
                        co_return co_await nxt::rt::with_zone(
                            nxt::rt::stop_on_success{},
                            [&](auto & policy)
                                -> nxt::rt::task<nxt::rt::deed<int>> {
                                auto child =
                                    policy.fork(value_after_yield(123));
                                policy.fork(
                                    record_stop_state_after_two_yields(
                                        events,
                                        3));
                                co_return std::move(child);
                            });
                    });

                expect(std::move(child).get() == 123_i);
                expect(events == std::vector<int>{3});
            };

            "wait_any returns the first successful task"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                auto result =
                    deck.sync_wait([&]() -> nxt::rt::task<int> {
                        co_return co_await nxt::rt::wait_any(
                            value_after_yield(5),
                            value_after_two_yields_or_stop(events, 6));
                    });

                expect(result == 5_i);
                expect(events == std::vector<int>{6});
            };

            "wait_any groups failures when all tasks fail"_test = [] {
                auto deck = nxt::rt::deck{};
                auto grouped = false;

                try {
                    (void)deck.sync_wait([]() -> nxt::rt::task<int> {
                        co_return co_await nxt::rt::wait_any(
                            throw_int_after_yield(),
                            throw_int_after_yield());
                    });
                } catch (const nxt::rt::exception_group & group) {
                    grouped = true;
                    expect(group.exceptions().size() == std::size_t{2});
                }

                expect(grouped);
            };

            "when_all returns a tuple of task results"_test = [] {
                auto deck = nxt::rt::deck{};

                auto values =
                    deck.sync_wait([]()
                        -> nxt::rt::task<std::tuple<int, std::string>> {
                        co_return co_await nxt::rt::when_all(
                            value_after_yield(7),
                            string_after_yield("seven"));
                    });

                expect(std::get<0>(values) == 7_i);
                expect(std::get<1>(values) == "seven");
            };

            "when_all stops siblings after a failure"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    (void)deck.sync_wait([&]() -> nxt::rt::task<
                        std::tuple<int, int>> {
                        co_return co_await nxt::rt::when_all(
                            throw_int_after_yield(),
                            value_after_two_yields_or_stop(events, 8));
                    });
                } catch (const std::exception &) {
                    threw = true;
                }

                expect(threw);
                expect(events == std::vector<int>{8});
            };
        };

        "wishes"_test = [] {
            "typed waiters are prepared and parked"_test = [] {
                auto wand = manual_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto events = std::vector<int>{};

                auto task_body =
                    [](std::vector<int> & events) -> nxt::rt::task<void> {
                    events.push_back(1);
                    co_await nxt::rt::manual_wish{.token = 42};
                    events.push_back(2);
                };

                auto task = task_body(events);
                deck.start(task);
                deck.run_ready();

                expect(events == std::vector<int>{1})
                    << "task should suspend before manual wish fulfillment";
                expect(deck.empty())
                    << "manual wish should not requeue itself";
                expect(
                    wand.prepared == std::vector<nxt::rt::wait_token>{42})
                    << "wand should synchronously prepare the wish";
                expect(wand.parked.size() == std::size_t{1})
                    << "waiter should park the suspended coroutine";
                expect(wand.parked.front().token == std::uint64_t{42});

                wand.fulfill(deck, 42);
                deck.run_ready();

                expect(events == std::vector<int>{1, 2})
                    << "fulfilled manual wish should resume the suspended task";
            };

            "the wand is waved after staged preparation"_test = [] {
                auto wand = manual_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto events = std::vector<int>{};

                auto task_body =
                    [](std::vector<int> & events) -> nxt::rt::task<void> {
                    events.push_back(1);
                    co_await nxt::rt::manual_wish{.token = 7};
                    events.push_back(2);
                };

                auto task = task_body(events);
                deck.start(task);
                deck.run_ready();

                expect(events == std::vector<int>{1});
                expect(
                    wand.prepared == std::vector<nxt::rt::wait_token>{7});
                expect(wand.parked.size() == std::size_t{1});
                expect(wand.waves == 1_i)
                    << "run_ready should wave the deck wand after the pump round";

                wand.fulfill(deck, 7);
                deck.run_ready();

                expect(events == std::vector<int>{1, 2});
                expect(wand.prepared.size() == std::size_t{1})
                    << "resuming after fulfillment should not prepare a second wish";
                expect(wand.waves == 2_i);
            };

            "stopped tasks request cancellation of parked wishes"_test = [] {
                auto wand = manual_wand{};
                auto deck = nxt::rt::deck{&wand};

                auto task = []() -> nxt::rt::task<void> {
                    co_await nxt::rt::manual_wish{.token = 99};
                }();

                deck.start(task);
                deck.run_ready();
                task.request_stop();

                expect(wand.cancelled == std::vector<nxt::rt::wait_token>{99});
            };
        };

        "buffers"_test = [] {
            "chunks are visited through reused storage"_test = [] {
                auto deck = nxt::rt::deck{};
                auto chunks = std::array{"ab"sv, "cdef"sv, "g"sv};
                auto source = nxt::rt::string_source{std::span{chunks}};
                auto storage = std::array<std::byte, 3>{};
                auto visited = std::vector<std::string>{};

                auto total =
                    deck.sync_wait([&]() -> nxt::rt::task<std::size_t> {
                        co_return co_await nxt::rt::for_each_chunk(
                            source,
                            std::span{storage},
                            [&visited](std::span<const std::byte> chunk) {
                                visited.emplace_back(
                                    nxt::rt::as_string_view(chunk));
                            });
                    });

                expect(total == std::size_t{7});
                expect(
                    visited == std::vector<std::string>{"abc", "def", "g"});
            };

            "protocol leftovers remain buffered"_test = [] {
                auto deck = nxt::rt::deck{};
                auto chunks = std::array{"abc--def--ghi"sv};
                auto source = nxt::rt::string_source{std::span{chunks}};
                auto storage = std::array<std::byte, 16>{};
                auto reader =
                    nxt::rt::byte_reader{source, std::span{storage}};

                auto parts = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::vector<std::string>> {
                        auto out = std::vector<std::string>{};
                        out.emplace_back(
                            nxt::rt::as_string_view(
                                co_await reader.take_until("--")));
                        out.emplace_back(
                            nxt::rt::as_string_view(
                                co_await reader.take_until("--")));
                        out.emplace_back(
                            nxt::rt::as_string_view(reader.buffered()));
                        co_return out;
                    });

                expect(
                    parts == std::vector<std::string>{"abc", "def", "ghi"});
            };

            "empty reads are distinguished from EOF"_test = [] {
                auto deck = nxt::rt::deck{};
                auto source = empty_then_string_source{};
                auto storage = std::array<std::byte, 8>{};
                auto reader =
                    nxt::rt::byte_reader{source, std::span{storage}};

                auto parts = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::vector<std::string>> {
                        auto out = std::vector<std::string>{};
                        while (auto chunk = co_await reader.take_some())
                            out.emplace_back(
                                nxt::rt::as_string_view(*chunk));
                        co_return out;
                    });

                expect(parts == std::vector<std::string>{"", "abc"});
            };
        };

        "pipes"_test = [] {
            "values are yielded to awaiting tasks"_test = [] {
                auto deck = nxt::rt::deck{};

                auto numbers = []() -> nxt::rt::pipe<int> {
                    co_yield 1;
                    co_yield 2;
                    co_yield 3;
                };

                auto values = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::vector<int>> {
                        auto pipe = numbers();
                        auto out = std::vector<int>{};
                        while (auto value = co_await pipe.next())
                            out.push_back(*value);
                        co_return out;
                    });

                expect(values == std::vector<int>{1, 2, 3});
            };

            "yielded values can be separated by awaits"_test = [] {
                auto deck = nxt::rt::deck{};

                auto paced_numbers = []() -> nxt::rt::pipe<int> {
                    co_yield 1;
                    co_await nxt::rt::yield();
                    co_yield 2;
                };

                auto values = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::vector<int>> {
                        auto pipe = paced_numbers();
                        auto out = std::vector<int>{};
                        while (auto value = co_await pipe.next())
                            out.push_back(*value);
                        co_return out;
                    });

                expect(values == std::vector<int>{1, 2});
            };

            "child tasks can be awaited before values are yielded"_test = [] {
                auto deck = nxt::rt::deck{};

                auto child = [](int value) -> nxt::rt::task<int> {
                    co_await nxt::rt::yield();
                    co_return value * 2;
                };

                auto doubled = [child]() -> nxt::rt::pipe<int> {
                    co_yield co_await child(2);
                    co_yield co_await child(3);
                };

                auto values = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::vector<int>> {
                        auto pipe = doubled();
                        auto out = std::vector<int>{};
                        while (auto value = co_await pipe.next())
                            out.push_back(*value);
                        co_return out;
                    });

                expect(values == std::vector<int>{4, 6});
            };

            "wishes can be awaited before more values are yielded"_test = [] {
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

            "exceptions propagate to awaiting tasks"_test = [] {
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

        "HTTP bodies"_test = [] {
            "the next response remains buffered after chunked bodies"_test = [] {
                auto deck = nxt::rt::deck{};
                auto chunks = std::array{
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"
                    "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"sv,
                };
                auto source = nxt::rt::string_source{std::span{chunks}};
                auto storage = std::array<std::byte, 256>{};
                auto reader =
                    nxt::rt::byte_reader{source, std::span{storage}};

                auto result =
                    deck.sync_wait([&]() -> nxt::rt::task<std::string> {
                        auto first =
                            co_await nxt::rt::http::read_response_head(
                                reader);
                        expect(first.status == 200_i);
                        expect(nxt::rt::http::is_chunked(first));

                        auto body = nxt::rt::http::read_response_body(
                            reader, first);
                        auto text = std::string{};
                        while (auto chunk = co_await body.next())
                            text += nxt::rt::as_string_view(*chunk);

                        auto second =
                            co_await nxt::rt::http::read_response_head(
                                reader);
                        expect(second.status == 204_i);
                        expect(
                            nxt::rt::http::content_length(second)
                            == std::size_t{0});
                        co_return text;
                    });

                expect(result == "hello world");
            };

            "the next response remains buffered after content-length bodies"_test =
                [] {
                    auto deck = nxt::rt::deck{};
                    auto chunks = std::array{
                        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
                        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n"sv,
                    };
                    auto source = nxt::rt::string_source{std::span{chunks}};
                    auto storage = std::array<std::byte, 128>{};
                    auto reader =
                        nxt::rt::byte_reader{source, std::span{storage}};

                    auto result =
                        deck.sync_wait([&]() -> nxt::rt::task<std::string> {
                            auto first =
                                co_await nxt::rt::http::read_response_head(
                                    reader);
                            expect(first.status == 200_i);

                            auto body = nxt::rt::http::read_response_body(
                                reader, first);
                            auto text = std::string{};
                            while (auto chunk = co_await body.next())
                                text += nxt::rt::as_string_view(*chunk);

                            auto second =
                                co_await nxt::rt::http::read_response_head(
                                    reader);
                            expect(second.status == 201_i);
                            co_return text;
                        });

                    expect(result == "hello");
                };
        };
    }};

} // namespace nxt::test
