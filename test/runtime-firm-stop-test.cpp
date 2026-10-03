#include "runtime-test.hpp"

namespace nxt::test {

void declare_runtime_firm_stop_tests()
{
    "share a stop token with forked children"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};

        deck.sync_wait([&]() -> nxtrt::task<void> {
            co_await nxtrt::with_firm(
                [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                    scope.fork(
                        record_stop_state_after_yield(events, 1));
                    scope.stop();
                    co_await scope.join();
                    co_return;
                });
        });

        expect(events == std::vector<int>{1});
    };

    "reject fork after firm stop"_test = [] {
        auto deck = nxtrt::deck{};
        auto rejected = false;

        deck.sync_wait([&]() -> nxtrt::task<void> {
            co_await nxtrt::with_firm(
                [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                    scope.stop();
                    try {
                        scope.fork(value_after_yield(1));
                    } catch (const std::exception &) {
                        rejected = true;
                    }
                    co_return;
                });
        });

        expect(rejected);
    };

    "request child stop when the firm body fails"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};
        auto threw = false;

        try {
            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await nxtrt::with_firm(
                    [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                        scope.fork(record_stop_state_after_yield(
                            events, 2));
                        throw nxtrt::runtime_error{
                            "firm body boom"};
                    });
            });
        } catch (const std::exception &) {
            threw = true;
        }

        expect(threw);
        expect(events == std::vector<int>{2});
    };

    "request task stop on forked children when the firm stops"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};

        deck.sync_wait([&]() -> nxtrt::task<void> {
            co_await nxtrt::with_firm(
                [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                    scope.fork(record_task_stop_state_after_yield(
                        events, 3));
                    scope.stop();
                    co_await scope.join();
                    co_return;
                });
        });

        expect(events == std::vector<int>{3});
    };

    "child cancellation is not a firm failure after stop"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};

        deck.sync_wait([&]() -> nxtrt::task<void> {
            co_await nxtrt::with_firm(
                [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                    scope.fork(
                        value_after_two_yields_or_stop(events, 4));
                    co_await nxtrt::yield();
                    scope.stop();
                    co_await scope.join();
                });
        });

        expect(events == std::vector<int>{4});
    };

    "child cancellation remains failure before firm stop"_test = [] {
        auto deck = nxtrt::deck{};
        auto threw = false;

        try {
            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await nxtrt::with_firm(
                    [](nxtrt::firm & scope) -> nxtrt::task<void> {
                        scope.fork([]() -> nxtrt::task<void> {
                            throw nxtrt::operation_cancelled{};
                        }());
                        co_await scope.join();
                        co_return;
                    });
            });
        } catch (const nxtrt::operation_cancelled &) {
            threw = true;
        }

        expect(threw);
    };

    "shielded child tasks do not inherit parent stop"_test = [] {
        auto deck = nxtrt::deck{};
        auto root = nxtrt::root_task{
            deck,
            [] {
                return shielded_child_stop_state();
            },
        };

        root.start();
        root.inner().request_stop();
        deck.run_until_idle();

        expect(root.inner().done());
        expect(!std::move(root.inner()).result());
    };

    "shielded child wishes are not cancelled by parent stop"_test = [] {
        auto wand = manual_wand{};
        auto deck = nxtrt::deck{&wand};
        auto root = nxtrt::root_task{
            deck,
            [] {
                return shielded_manual_token(99);
            },
        };

        root.start();
        root.inner().request_stop();
        deck.run_until_idle();

        expect(wand.prepared == std::vector<nxtrt::coin_t>{99});
        expect(wand.cancelled.empty());
        expect(wand.parked.size() == std::size_t{1});

        wand.fulfill(deck, 99);
        deck.run_until_idle();

        expect(root.inner().done());
        std::move(root.inner()).result();
    };

    "stop a hosted firm when its parent task is stopped"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};

        auto root = nxtrt::root_task{
            deck,
            [&] {
                return hosted_firm_stop_probe(events);
            },
        };

        root.start();
        for (auto i = 0; i != 8 && events.empty(); ++i)
            deck.run_ready();

        expect(events == std::vector<int>{100});
        root.inner().request_stop();
        deck.run_until_idle();

        expect(events == std::vector<int>{100, 4});
    };

    "stop-on-failure firm stops siblings"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};
        auto threw = false;

        try {
            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await nxtrt::with_firm<nxtrt::stop_on_failure>(
                    [&](auto & policy) -> nxtrt::task<void> {
                        policy.fork(throw_after_yield(events, 1));
                        policy.fork(
                            record_stop_state_after_two_yields(
                                events,
                                2));
                        co_await policy.join();
                        co_return;
                    });
            });
        } catch (const std::exception &) {
            threw = true;
        }

        expect(threw);
        expect(events == std::vector<int>{11, 2});
    };

    "stop-on-success firm stops siblings"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};

        auto child =
            deck.sync_wait([&]() -> nxtrt::task<nxtrt::deed<int>> {
                co_return co_await nxtrt::with_firm<
                    nxtrt::stop_on_success>(
                    [&](auto & policy)
                        -> nxtrt::task<nxtrt::deed<int>> {
                        auto child =
                            policy.fork(value_after_yield(123));
                        policy.fork(
                            record_stop_state_after_two_yields(
                                events,
                                3));
                        co_await policy.join();
                        co_return std::move(child);
                    });
            });

        expect(std::move(child).get() == 123_i);
        expect(events == std::vector<int>{3});
    };

    "wait_any returns the first successful task"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};

        auto result =
            deck.sync_wait([&]() -> nxtrt::task<int> {
                co_return co_await nxtrt::wait_any(
                    value_after_yield(5),
                    value_after_two_yields_or_stop(events, 6));
            });

        expect(result == 5_i);
        expect(events == std::vector<int>{6});
    };

    "wait_any groups failures when all tasks fail"_test = [] {
        auto deck = nxtrt::deck{};
        auto grouped = false;

        try {
            (void)deck.sync_wait([]() -> nxtrt::task<int> {
                co_return co_await nxtrt::wait_any(
                    throw_int_after_yield(),
                    throw_int_after_yield());
            });
        } catch (const nxtrt::exception_group & group) {
            grouped = true;
            expect(group.exceptions().size() == std::size_t{2});
        }

        expect(grouped);
    };

    "when_all returns a tuple of task results"_test = [] {
        auto deck = nxtrt::deck{};

        auto values =
            deck.sync_wait([]()
                -> nxtrt::task<std::tuple<int, std::string>> {
                co_return co_await nxtrt::when_all(
                    value_after_yield(7),
                    string_after_yield("seven"));
            });

        expect(std::get<0>(values) == 7_i);
        expect(std::get<1>(values) == "seven");
    };

    "when_all stops siblings after a failure"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};
        auto threw = false;

        try {
            (void)deck.sync_wait([&]() -> nxtrt::task<
                std::tuple<int, int>> {
                co_return co_await nxtrt::when_all(
                    throw_int_after_yield(),
                    value_after_two_yields_or_stop(events, 8));
            });
        } catch (const std::exception &) {
            threw = true;
        }

        expect(threw);
        expect(events == std::vector<int>{8});
    };

    "moved joined deeds survive nursery destruction"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto deeds = deck.sync_wait([] {
                return nxtrt::run_firm(
                    returned_deeds_firm{});
            });
            {
                auto first = std::move(std::get<0>(deeds));
                auto second = std::move(std::get<1>(deeds));
                expect(std::move(first).get() == 41);
                std::move(second).get();
            }
        };

    "rejoining does not repeat failures from released observed deeds"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            deck.sync_wait([&] {
                return nxtrt::with_firm(
                    [&](nxtrt::firm & scope) -> nxtrt::task<void> {
                        {
                            auto first =
                                scope.fork(throw_int_after_yield())
                                    .cope();
                            auto second =
                                scope
                                    .fork(throw_after_yield(
                                        events, 13))
                                    .cope();
                            co_await scope.join();
                            expect(!std::move(first).get());
                            expect(!std::move(second).get());
                        }
                        co_await scope.join();
                    });
            });
            expect(events == std::vector<int>{131});
        };

    "tuple all owns factories and collects heterogeneous results"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto calls = 0;
            auto values = deck.sync_wait([&] {
                return nxtrt::when_all(
                    std::tuple{
                        // Deliberately test a capturing coroutine
                        // factory:
                        // its move-only closure must survive
                        // suspension.
                        [value = std::make_unique<int>(29),
                         &calls]()
                            -> nxtrt::task<std::unique_ptr<int>> {
                            ++calls;
                            auto & scope = *nxtrt::current_firm();
                            // Main tuple work is pool-owned: no
                            // nursery child/deed records.
                            expect(scope.child_count() == 0);
                            co_await nxtrt::yield();
                            co_return std::make_unique<int>(*value);
                        },
                        value_after_yield(7),
                        [&] {
                            return record_after_yield(events, 5);
                        },
                    });
            });
            static_assert(std::same_as<
                          decltype(values),
                          std::tuple<
                              std::unique_ptr<int>,
                              int,
                              std::monostate>>);
            expect(*std::get<0>(values) == 29);
            expect(std::get<1>(values) == 7);
            expect(calls == 1);
            expect(events == std::vector<int>{51, 52});
        };

    "tuple factories are lazy and run inside their receiving firm"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto calls = 0;
            auto result = deck.sync_wait(
                [&]() -> nxtrt::task<std::tuple<int>> {
                    auto * parent = nxtrt::current_firm();
                    auto work =
                        nxtrt::when_all(std::tuple{[&, parent] {
                            ++calls;
                            expect(nxtrt::current_firm() != parent);
                            expect(nxtrt::current_firm() != nullptr);
                            return value_after_yield(53);
                        }});
                    expect(calls == 0);
                    co_return co_await std::move(work);
                });
            expect(calls == 1 && std::get<0>(result) == 53);
            auto empty = deck.sync_wait(
                [] { return nxtrt::when_all(std::tuple{}); });
            static_assert(std::tuple_size_v<decltype(empty)> == 0);
        };

    "tuple first success skips failures and drains losers"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto result = deck.sync_wait([&] {
                return nxtrt::wait_any(
                    std::tuple{
                        throw_int_after_yield,
                        [] { return value_after_yield(37); },
                        [&] {
                            return tuple_wait_for_stop(events, 19);
                        },
                    });
            });
            expect(result == 37);
            expect(events == std::vector<int>{19});
            auto grouped = false;
            try {
                (void) deck.sync_wait([] {
                    return nxtrt::wait_any(
                        std::tuple{
                            throw_int_after_yield,
                            throw_int_after_yield});
                });
            } catch (const nxtrt::exception_group & group) {
                grouped = true;
                expect(group.exceptions().size() == 2);
            }
            expect(grouped);
            deck.sync_wait([] {
                return nxtrt::wait_any(
                    std::tuple{
                        returned_deeds_firm::empty_child,
                        returned_deeds_firm::empty_child});
            });
        };

    "tuple all and first completion stop on failure"_test = [] {
        auto deck = nxtrt::deck{};
        auto events = std::vector<int>{};
        auto failed = false;
        try {
            (void) deck.sync_wait([&] {
                return nxtrt::when_all(
                    std::tuple{
                        throw_int_after_yield,
                        [&] {
                            return tuple_wait_for_stop(events, 11);
                        },
                    });
            });
        } catch (const nxtrt::runtime_error & error) {
            failed = std::string_view{error.what()}
                     == "firm child int boom";
        }
        expect(failed);
        expect(events == std::vector<int>{11});
        auto deeds = deck.sync_wait([&] {
            return nxtrt::with_firm<nxtrt::stop_on_completion>(
                std::tuple{
                    throw_int_after_yield,
                    [&] { return tuple_wait_for_stop(events, 23); },
                });
        });
        auto first = std::move(std::get<0>(deeds));
        auto second = std::move(std::get<1>(deeds));
        expect(!first && !second);
        expect(!nxtrt::is_operation_cancelled(first.error()));
        expect(nxtrt::is_operation_cancelled(second.error()));
        expect(events == std::vector<int>{11, 23});
    };

    "tuple factory failure retains and drains earlier factories"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto skipped = 0;
            auto failed = false;
            try {
                (void) deck.sync_wait([&] {
                    return nxtrt::when_all(
                        std::tuple{
                            [value = std::make_unique<int>(43),
                             &events]() -> nxtrt::task<int> {
                                co_await nxtrt::yield();
                                expect(nxtrt::stop_requested());
                                events.push_back(*value);
                                co_return *value;
                            },
                            []() -> nxtrt::task<int> {
                                throw std::domain_error{
                                    "factory failed"};
                            },
                            [&] {
                                ++skipped;
                                return value_after_yield(1);
                            },
                        });
                });
            } catch (const std::domain_error & error) {
                failed = std::string_view{error.what()}
                         == "factory failed";
            }
            expect(failed && skipped == 0);
            expect(events == std::vector<int>{43});
        };

    "tuple and variadic frames retain firm context without spawning"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto result = deck.sync_wait([&] {
                return nxtrt::when_all(
                    tuple_frame_context(events),
                    value_after_yield(7));
            });
            expect(
                std::get<0>(result) == 17
                && std::get<1>(result) == 7);
            expect(events == std::vector<int>{3});
            events.clear();
            result = deck.sync_wait([&] {
                return nxtrt::when_all(
                    std::tuple{
                        [&] { return tuple_frame_context(events); },
                        [] { return value_after_yield(7); },
                    });
            });
            expect(std::get<0>(result) == 17);
            expect(std::get<1>(result) == 7);
            expect(events == std::vector<int>{3});
        };

    "tuple outcomes retain positions and individual failures"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto outcomes = deck.sync_wait([&] {
                return nxtrt::with_firm(
                    std::tuple{
                        [&] {
                            return value_after_two_yields_or_stop(
                                events, 43);
                        },
                        [] { return value_after_yield(7); },
                        throw_int_after_yield,
                        returned_deeds_firm::empty_child,
                    });
            });
            static_assert(
                std::same_as<
                    decltype(outcomes),
                    std::tuple<
                        std::expected<int, std::exception_ptr>,
                        std::expected<int, std::exception_ptr>,
                        std::expected<int, std::exception_ptr>,
                        std::expected<void, std::exception_ptr>>>);
            expect(std::get<0>(outcomes).value() == -43);
            expect(std::get<1>(outcomes).value() == 7);
            expect(!std::get<2>(outcomes));
            expect(!nxtrt::is_operation_cancelled(
                std::get<2>(outcomes).error()));
            expect(std::get<3>(outcomes).has_value());
            expect(events.empty());
            auto values = deck.sync_wait([&] {
                return nxtrt::when_all(
                    record_after_yield(events, 9),
                    value_after_yield(31));
            });
            static_assert(std::same_as<
                          decltype(values),
                          std::tuple<std::monostate, int>>);
            expect(std::get<1>(values) == 31);
            expect(events == std::vector<int>{91, 92});
        };

    "stopping during a tuple factory settles unstarted positions"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto skipped = 0;
            auto outcomes = deck.sync_wait([&] {
                return nxtrt::with_firm(
                    std::tuple{
                        [] {
                            nxtrt::current_firm()->stop();
                            return value_after_yield(17);
                        },
                        [&] {
                            ++skipped;
                            return value_after_yield(29);
                        },
                    });
            });
            expect(skipped == 0);
            expect(
                !std::get<0>(outcomes) && !std::get<1>(outcomes));
            expect(
                nxtrt::is_operation_cancelled(
                    std::get<0>(outcomes).error()));
            expect(
                nxtrt::is_operation_cancelled(
                    std::get<1>(outcomes).error()));
        };

    "tuple recipes reject empty tasks before awaiting them"_test =
        [] {
            auto deck = nxtrt::deck{};
            auto rejected = false;
            try {
                (void) deck.sync_wait([] {
                    return nxtrt::when_all(std::tuple{[] {
                        return nxtrt::task<int>{};
                    }});
                });
            } catch (const nxtrt::runtime_error & error) {
                rejected =
                    std::string_view{error.what()}
                    == "nxtrt tuple recipe returned an empty task";
            }
            expect(rejected);
        };

    "tuple all accepts cancellation at every startup turn"_test =
        [] {
            auto saw_prepared = false;
            auto saw_started = false;
            for (int turns = 0; turns < 40; ++turns) {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto calls = 0;
                auto starts = 0;
                auto root = nxtrt::root_task{
                    deck, [&] {
                        return nxtrt::when_all(std::tuple{[&] {
                            ++calls;
                            return tuple_wait_for_stop(
                                events, 61, &starts);
                        }});
                    }};
                root.start();
                for (int i = 0; i < turns; ++i)
                    deck.run_ready();
                root.inner().request_stop();
                deck.run_until_idle();
                expect(root.inner().done());
                auto cancelled = false;
                try {
                    (void) std::move(root.inner()).result();
                } catch (const nxtrt::operation_cancelled &) {
                    cancelled = true;
                }
                expect(cancelled);
                if (turns == 0)
                    expect(calls == 0);
                expect(calls <= 1);
                expect(starts <= calls);
                saw_prepared |= calls != 0 && starts == 0;
                saw_started |= starts != 0;
                expect(
                    events
                    == (starts ? std::vector<int>{61}
                               : std::vector<int>{}));
            }
            expect(saw_prepared && saw_started);
        };
}

} // namespace nxt::test
