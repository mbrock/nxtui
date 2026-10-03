#include "runtime-test.hpp"

namespace nxt::test {

namespace {

template<typename T>
concept tuple_settle_input = requires(T input) {
    nxtrt::settle(std::tuple{std::move(input)});
};

template<typename T>
concept tuple_when_all_input = requires(T input) {
    nxtrt::when_all(std::tuple{std::move(input)});
};

template<typename T>
concept tuple_wait_any_input = requires(T input) {
    nxtrt::wait_any(std::tuple{std::move(input)});
};

template<typename T>
concept range_settle_input = requires(std::vector<T> input) {
    nxtrt::settle_range(std::move(input));
};

using int_task_factory = decltype(&throw_int_after_yield);
static_assert(tuple_settle_input<nxtrt::task<int>>);
static_assert(tuple_when_all_input<nxtrt::task<int>>);
static_assert(tuple_wait_any_input<nxtrt::task<int>>);
static_assert(range_settle_input<nxtrt::task<int>>);
static_assert(!tuple_settle_input<int_task_factory>);
static_assert(!tuple_when_all_input<int_task_factory>);
static_assert(!tuple_wait_any_input<int_task_factory>);
static_assert(!range_settle_input<int_task_factory>);

nxtrt::task<std::unique_ptr<int>> owned_value_after_yield(
    std::unique_ptr<int> value, int & starts)
{
    ++starts;
    co_await nxtrt::yield();
    co_return std::move(value);
}

struct drain_probe
{
    int active = 0;
    int peak = 0;
    int finished = 0;
};

nxtrt::task<void> run_drain_job(drain_probe & probe)
{
    ++probe.active;
    probe.peak = std::max(probe.peak, probe.active);
    co_await nxtrt::yield();
    co_await nxtrt::yield();
    --probe.active;
    ++probe.finished;
}

struct drain_idea
{
    drain_probe * probe = nullptr;

    nxtrt::task<void> operator()() &
    {
        return run_drain_job(*probe);
    }
};

} // namespace

void declare_runtime_group_tests()
{
    "groups"_group = [] {
        "settle returns outcomes in order with individual failures"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto outcomes = deck.sync_wait([&] {
                    return nxtrt::settle(std::tuple{
                        value_after_two_yields_or_stop(events, 43),
                        value_after_yield(7),
                        throw_int_after_yield(),
                        empty_child(),
                    });
                });
                static_assert(std::same_as<
                              decltype(outcomes),
                              std::tuple<
                                  nxtrt::outcome<int>,
                                  nxtrt::outcome<int>,
                                  nxtrt::outcome<int>,
                                  nxtrt::outcome<void>>>);
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

        "stop on failure stops the others"_test = []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};
            auto outcomes = co_await nxtrt::settle(
            std::tuple{
                throw_after_yield(events, 1),
                record_stop_state_after_two_yields(events, 2),
            },
            nxtrt::stop_on_failure{});
            expect(!std::get<0>(outcomes));
            expect(std::get<1>(outcomes).has_value());
            expect(events == std::vector<int>{11, 2});
        };

        "stop on success stops the others"_test = []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};
            auto outcomes = co_await nxtrt::settle(
            std::tuple{
                value_after_yield(123),
                record_stop_state_after_two_yields(events, 3),
            },
            nxtrt::stop_on_success{});
            expect(std::get<0>(outcomes).value() == 123);
            expect(events == std::vector<int>{3});
        };

        "stop after first stops companions when the primary settles"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto outcomes = deck.sync_wait([&] {
                    return nxtrt::settle(
                        std::tuple{
                            value_after_yield(5),
                            tuple_wait_for_stop(events, 6),
                        },
                        nxtrt::stop_after_first{});
                });
                expect(std::get<0>(outcomes).value() == 5);
                expect(!std::get<1>(outcomes));
                expect(nxtrt::is_operation_cancelled(
                    std::get<1>(outcomes).error()));
                expect(events == std::vector<int>{6});

                // A failing companion does not stop the primary.
                events.clear();
                auto primary_ran = deck.sync_wait([&] {
                    return nxtrt::settle(
                        std::tuple{
                            value_after_two_yields_or_stop(events, 7),
                            throw_int_after_yield(),
                        },
                        nxtrt::stop_after_first{});
                });
                expect(std::get<0>(primary_ran).value() == -7);
                expect(!std::get<1>(primary_ran));
                expect(events.empty());
            };

        "a stop rule can be any noexcept callable"_test = []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};
            auto outcomes = co_await nxtrt::settle(
            std::tuple{
                tuple_wait_for_stop(events, 1),
                value_after_yield(2),
            },
            [](std::size_t index, bool) noexcept {
                return index == 1;
            });
            expect(!std::get<0>(outcomes));
            expect(std::get<1>(outcomes).value() == 2);
            expect(events == std::vector<int>{1});
        };

        "wait_any returns the first successful task"_test = []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};

            int result = co_await nxtrt::wait_any(
                value_after_yield(5),
                value_after_two_yields_or_stop(events, 6));

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

        "wait_any skips failures and drains losers"_test = []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};
            auto result = co_await nxtrt::wait_any(std::tuple{
                throw_int_after_yield(),
                value_after_yield(37),
                tuple_wait_for_stop(events, 19),
            });
            expect(result == 37);
            expect(events == std::vector<int>{19});
            co_await nxtrt::wait_any(std::tuple{empty_child(), empty_child()});
        };

        "when_all returns a tuple of task results"_test = []() -> nxtrt::task<void> {
            std::tuple<int, std::string> values = co_await nxtrt::when_all(
                value_after_yield(7),
                string_after_yield("seven"));

            expect(std::get<0>(values) == 7_i);
            expect(std::get<1>(values) == "seven");
        };

        "when_all stops the others after a failure"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto failed = false;
            try {
                (void)deck.sync_wait([&] {
                    return nxtrt::when_all(std::tuple{
                        throw_int_after_yield(),
                        tuple_wait_for_stop(events, 11),
                    });
                });
            } catch (const nxtrt::runtime_error & error) {
                failed =
                    std::string_view{error.what()} == "firm child int boom";
            }
            expect(failed);
            expect(events == std::vector<int>{11});

            auto outcomes = deck.sync_wait([&] {
                return nxtrt::settle(
                    std::tuple{
                        throw_int_after_yield(),
                        tuple_wait_for_stop(events, 23),
                    },
                    nxtrt::stop_on_completion{});
            });
            auto first = std::move(std::get<0>(outcomes));
            auto second = std::move(std::get<1>(outcomes));
            expect(!first && !second);
            expect(!nxtrt::is_operation_cancelled(first.error()));
            expect(nxtrt::is_operation_cancelled(second.error()));
            expect(events == std::vector<int>{11, 23});
        };

        "when_all owns tasks and collects mixed results"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto calls = 0;
                auto values = deck.sync_wait([&] {
                    return nxtrt::when_all(std::tuple{
                        owned_value_after_yield(
                            std::make_unique<int>(29), calls),
                        value_after_yield(7),
                        record_after_yield(events, 5),
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

        "group tasks stay lazy until awaited"_test = []() -> nxtrt::task<void> {
            auto starts = 0;
            auto child = owned_value_after_yield(
                std::make_unique<int>(53), starts);
            expect(starts == 0);
            auto work = nxtrt::when_all(std::tuple{std::move(child)});
            expect(!child.handle());
            expect(starts == 0);
            auto result = co_await std::move(work);
            expect(starts == 1 && *std::get<0>(result) == 53);
        };

        "empty groups complete without jobs"_test = []() -> nxtrt::task<void> {
            auto values = co_await nxtrt::when_all(std::tuple{});
            auto outcomes = co_await nxtrt::settle(std::tuple{});
            static_assert(std::tuple_size_v<decltype(values)> == 0);
            static_assert(std::tuple_size_v<decltype(outcomes)> == 0);
            auto ranged = co_await nxtrt::settle_range(
                std::vector<nxtrt::task<int>>{});
            expect(ranged.empty());
        };

        "groups reject empty tasks before awaiting them"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto rejected = false;
            try {
                (void)deck.sync_wait([&] {
                    return nxtrt::when_all(std::tuple{
                        tuple_wait_for_stop(events, 43),
                        nxtrt::task<int>{},
                    });
                });
            } catch (const nxtrt::runtime_error & error) {
                rejected = std::string_view{error.what()}
                           == "nxtrt group received an empty task";
            }
            expect(rejected);
            expect(events == std::vector<int>{43});
        };

        "when_all accepts cancellation at every startup turn"_test = [] {
            auto saw_unstarted = false;
            auto saw_started = false;
            for (int turns = 0; turns < 40; ++turns) {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto starts = 0;
                auto child = tuple_wait_for_stop(events, 61, &starts);
                expect(starts == 0);
                auto root = nxtrt::root_task{deck, [&] {
                    return nxtrt::when_all(std::tuple{std::move(child)});
                }};
                root.start();
                for (int i = 0; i < turns; ++i)
                    deck.run_ready();
                root.inner().request_stop();
                deck.run_until_idle();
                expect(root.inner().done());
                auto cancelled = false;
                try {
                    (void)std::move(root.inner()).result();
                } catch (const nxtrt::operation_cancelled &) {
                    cancelled = true;
                }
                expect(cancelled);
                if (turns == 0)
                    expect(starts == 0);
                expect(starts <= 1);
                saw_unstarted |= starts == 0;
                saw_started |= starts != 0;
                expect(
                    events
                    == (starts ? std::vector<int>{61} : std::vector<int>{}));
            }
            expect(saw_unstarted && saw_started);
        };

        "stopping the awaiting task stops the group"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};

            auto root = nxtrt::root_task{
                deck,
                [&] {
                    return hosted_group_stop_probe(events);
                },
            };

            root.start();
            for (auto i = 0; i != 32 && events.empty(); ++i)
                deck.run_ready();

            expect(events == std::vector<int>{100});
            root.inner().request_stop();
            deck.run_until_idle();

            expect(events == std::vector<int>{100, 4});
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

        "settle_range returns outcomes in range order"_test = [] {
            auto deck = nxtrt::deck{};
            auto outcomes = deck.sync_wait([] {
                auto work = std::vector<nxtrt::task<int>>{};
                work.push_back(value_after_yield(1));
                work.push_back(throw_int_after_yield());
                work.push_back(value_after_yield(3));
                return nxtrt::settle_range(std::move(work));
            });
            expect(outcomes.size() == std::size_t{3});
            expect(outcomes[0].value() == 1);
            expect(!outcomes[1]);
            expect(outcomes[2].value() == 3);

            auto events = std::vector<int>{};
            auto stopped = deck.sync_wait([&] {
                auto work = std::vector<nxtrt::task<int>>{};
                work.push_back(tuple_wait_for_stop(events, 5));
                work.push_back(throw_int_after_yield());
                return nxtrt::settle_range(
                    std::move(work), nxtrt::stop_on_failure{});
            });
            expect(nxtrt::is_operation_cancelled(stopped[0].error()));
            expect(!nxtrt::is_operation_cancelled(stopped[1].error()));
            expect(events == std::vector<int>{5});
        };

        "drain runs a feed of ideas with bounded concurrency"_test = []() -> nxtrt::task<void> {
            auto probe = drain_probe{};
            auto ideas = std::vector<drain_idea>(10, drain_idea{&probe});
            auto input = nxtrt::value_range_source{ideas};
            co_await nxtrt::drain(input, 3);
            expect(probe.finished == 10);
            expect(probe.peak == 3);
            expect(probe.active == 0);
        };
    };
}

} // namespace nxt::test
