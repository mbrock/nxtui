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

template<typename Policy>
concept settle_policy = requires(Policy policy, nxtrt::task<int> child) {
    nxtrt::settle(std::tuple{std::move(child)}, std::move(policy));
};

static_assert(settle_policy<decltype([](std::size_t, bool) noexcept {
    return false;
})>);
static_assert(
    !settle_policy<decltype([](std::size_t, bool) { return false; })>);

template<typename Fn>
concept completion_callback = requires(nxtrt::task<int> & child, Fn fn) {
    child.on_completed(std::move(fn));
};

static_assert(completion_callback<decltype([]() noexcept {})>);
static_assert(!completion_callback<decltype([] {})>);
static_assert(!completion_callback<decltype([]() noexcept { return 1; })>);

nxtrt::task<std::unique_ptr<int>> owned_value_after_yield(
    std::unique_ptr<int> value, int & starts)
{
    ++starts;
    co_await nxtrt::yield();
    co_return std::move(value);
}

struct extraction_value
{
    bool * fail_moves;

    explicit extraction_value(bool & fail)
        : fail_moves(&fail)
    {}

    extraction_value(const extraction_value &) = delete;

    extraction_value(extraction_value && other)
        : fail_moves(other.fail_moves)
    {
        if (*fail_moves)
            throw std::domain_error{"result extraction failed"};
    }
};

nxtrt::task<extraction_value> value_before_extraction(bool & fail_moves)
{
    co_return extraction_value{fail_moves};
}

nxtrt::task<void> fail_later_moves(bool & fail_moves)
{
    co_await nxtrt::yield();
    fail_moves = true;
}

nxtrt::task<void> observe_stop_turns(std::vector<bool> & states)
{
    states.push_back(nxtrt::stop_requested());
    co_await nxtrt::yield();
    states.push_back(nxtrt::stop_requested());
}

struct indexed_group
{
    std::size_t index;

    explicit indexed_group(std::size_t index) : index(index) {}

    bool operator()(std::size_t settled, bool) const noexcept
    {
        return settled == index;
    }
};

struct observed_failure_group
{
    int & failures;
    int & completions;

    observed_failure_group(int & failures, int & completions)
        : failures(failures), completions(completions)
    {}

    bool operator()(std::size_t, bool failed) const noexcept
    {
        ++completions;
        failures += failed;
        return failed;
    }
};

struct later_move_value
{
    int moves_left = 2;

    later_move_value() = default;
    later_move_value(const later_move_value &) = delete;
    later_move_value(later_move_value && other)
        : moves_left(other.moves_left - 1)
    {
        if (moves_left < 0)
            throw std::domain_error{"aggregate move failed"};
    }
};

nxtrt::task<later_move_value> value_with_later_move_failure()
{
    co_return later_move_value{};
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
            nxtrt::fail_fast_group{});
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
            nxtrt::first_success_group{});
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
                        nxtrt::primary_group{});
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
                        nxtrt::primary_group{});
                });
                expect(std::get<0>(primary_ran).value() == -7);
                expect(!std::get<1>(primary_ran));
                expect(events.empty());
            };

        "a policy can choose a configured child"_test =
            []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};
            auto outcomes = co_await nxtrt::settle(
                std::tuple{
                    tuple_wait_for_stop(events, 1),
                    value_after_yield(2),
                },
                indexed_group{1});
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
                    nxtrt::first_completion_group{});
            });
            auto first = std::move(std::get<0>(outcomes));
            auto second = std::move(std::get<1>(outcomes));
            expect(!first && !second);
            expect(!nxtrt::is_operation_cancelled(first.error()));
            expect(nxtrt::is_operation_cancelled(second.error()));
            expect(events == std::vector<int>{11, 23});
        };

        "when_all rethrows the failure that stopped the others"_test =
            []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};
            auto message = std::string{};
            try {
                (void)co_await nxtrt::when_all(
                    tuple_wait_for_stop(events, 11), throw_int_after_yield());
            } catch (const nxtrt::runtime_error & error) {
                message = error.what();
            }
            expect(message == "firm child int boom");
            expect(events == std::vector<int>{11});

            message.clear();
            auto tasks = std::vector<nxtrt::task<int>>{};
            tasks.push_back(tuple_wait_for_stop(events, 12));
            tasks.push_back(tuple_wait_for_stop(events, 13));
            tasks.push_back(throw_int_after_yield());
            try {
                (void)co_await nxtrt::when_all_range(std::move(tasks));
            } catch (const nxtrt::runtime_error & error) {
                message = error.what();
            }
            expect(message == "firm child int boom");
            expect(events == std::vector<int>{11, 12, 13});
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

        "groups reject empty tasks before starting children"_test = [] {
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
            expect(events.empty());
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
                    std::move(work), nxtrt::fail_fast_group{});
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

        "groups read already completed tasks without replaying completion"_test = [] {
            auto deck = nxtrt::deck{};
            auto first = value_after_yield(17);
            auto second = value_after_yield(23);
            deck.start(first);
            deck.start(second);
            deck.run_until_idle();
            expect(first.done() && second.done());

            auto outcomes = deck.sync_wait([&] {
                return nxtrt::settle(
                    std::tuple{std::move(first), std::move(second)},
                    nxtrt::first_completion_group{});
            });
            expect(std::get<0>(outcomes).value() == 17);
            expect(std::get<1>(outcomes).value() == 23);
        };

        "completed failure stops unstarted tasks"_test = [] {
            auto deck = nxtrt::deck{};
            auto failed = throw_int_after_yield();
            deck.start(failed);
            deck.run_until_idle();
            expect(failed.done());

            auto starts = 0;
            auto outcomes = deck.sync_wait([&] {
                return nxtrt::settle(
                    std::tuple{
                        std::move(failed),
                        owned_value_after_yield(
                            std::make_unique<int>(29), starts),
                    },
                    nxtrt::fail_fast_group{});
            });
            expect(!std::get<0>(outcomes));
            expect(!std::get<1>(outcomes));
            expect(nxtrt::is_operation_cancelled(
                std::get<1>(outcomes).error()));
            expect(starts == 0);
        };

        "result extraction failure does not change completion policy"_test =
            []() -> nxtrt::task<void> {
                auto fail_moves = false;
                auto failures = 0;
                auto completions = 0;
                auto outcomes = co_await nxtrt::settle(
                    std::tuple{
                        value_before_extraction(fail_moves),
                        fail_later_moves(fail_moves),
                    },
                    observed_failure_group{failures, completions});
                expect(completions == 2);
                expect(failures == 0);
                expect(!std::get<0>(outcomes));
                expect(std::get<1>(outcomes).has_value());
                auto extraction_failed = false;
                try {
                    nxtrt::rethrow(std::get<0>(outcomes).error());
                } catch (const std::domain_error & error) {
                    extraction_failed =
                        std::string_view{error.what()} == "result extraction failed";
                }
                expect(extraction_failed);
            };

        "later result moves can throw only after children drain"_test =
            []() -> nxtrt::task<void> {
                auto events = std::vector<int>{};
                auto failed = false;
                try {
                    (void)co_await nxtrt::settle(std::tuple{
                        value_with_later_move_failure(),
                        record_after_yield(events, 7),
                    });
                } catch (const std::domain_error & error) {
                    failed = std::string_view{error.what()} == "aggregate move failed";
                }
                expect(failed);
                expect(events == std::vector<int>{71, 72});
            };

        "child startup failure drains scheduled children before rethrow"_test =
            [] {
                // settle is the root: it, run_group, and one child fit.
                // Scheduling the second child fails after the first is
                // queued.
                auto storage = nxtrt::static_deck_task_storage<3>{};
                auto deck = nxtrt::deck{storage};
                auto events = std::vector<int>{};
                auto failed = false;
                try {
                    (void) deck.sync_wait([&] {
                        auto tasks = std::vector<nxtrt::task<int>>{};
                        for (auto i = 0; i != 8; ++i)
                            tasks.push_back(tuple_wait_for_stop(events, i));
                        return nxtrt::settle_range(std::move(tasks));
                    });
                } catch (const nxtrt::runtime_error &) {
                    failed = true;
                }
                expect(failed);
                expect(events == std::vector<int>{0});
                expect(deck.empty());
            };

        "link registration failure drains scheduled children"_test = [] {
            auto deck = nxtrt::deck{};
            auto events = std::vector<int>{};
            auto observed = value_after_yield(23);
            auto calls = 0;
            auto link = observed.on_completed([&]() noexcept { ++calls; });
            auto failed = false;
            try {
                (void) deck.sync_wait([&] {
                    return nxtrt::settle(
                        std::tuple{
                            tuple_wait_for_stop(events, 41),
                            std::move(observed),
                        });
                });
            } catch (const nxtrt::runtime_error & error) {
                failed = std::string_view{error.what()}
                         == "nxtrt task already has a completion observer";
            }
            expect(failed);
            expect(events == std::vector<int>{41});
            expect(calls == 0 && !link.connected());
            expect(deck.empty());
        };

        "policies work with ranges"_test = []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};
            auto tasks = std::vector<nxtrt::task<int>>{};
            tasks.push_back(tuple_wait_for_stop(events, 9));
            tasks.push_back(value_after_yield(11));
            auto outcomes = co_await nxtrt::settle_range(
                std::move(tasks), indexed_group{1});
            expect(nxtrt::is_operation_cancelled(outcomes[0].error()));
            expect(outcomes[1].value() == 11);
            expect(events == std::vector<int>{9});
        };

        "startup failure before any child starts does not wait"_test = [] {
            // Only settle and run_group fit; no child can be scheduled.
            auto storage = nxtrt::static_deck_task_storage<2>{};
            auto deck = nxtrt::deck{storage};
            auto events = std::vector<int>{};
            auto failed = false;
            try {
                (void) deck.sync_wait([&] {
                    return nxtrt::settle(
                        std::tuple{
                            tuple_wait_for_stop(events, 41),
                            tuple_wait_for_stop(events, 23),
                        });
                });
            } catch (const nxtrt::runtime_error & error) {
                failed = std::string_view{error.what()}
                         == "nxtrt deck task table is full";
            }
            expect(failed);
            expect(events.empty());
            expect(deck.empty());
        };

        "a lambda policy needs no group object"_test =
            []() -> nxtrt::task<void> {
            auto events = std::vector<int>{};
            auto trigger = std::size_t{1};
            auto outcomes = co_await nxtrt::settle(
                std::tuple{
                    tuple_wait_for_stop(events, 13),
                    value_after_yield(37),
                },
                [trigger](std::size_t index, bool) noexcept {
                    return index == trigger;
                });
            expect(
                nxtrt::is_operation_cancelled(
                    std::get<0>(outcomes).error()));
            expect(std::get<1>(outcomes).value() == 37);
            expect(events == std::vector<int>{13});
        };

        "policy stop runs at child final suspension"_test =
            []() -> nxtrt::task<void> {
            auto states = std::vector<bool>{};
            auto finish = []() -> nxtrt::task<void> { co_return; };
            auto outcomes = co_await nxtrt::settle(
                std::tuple{finish(), observe_stop_turns(states)},
                nxtrt::first_completion_group{});
            // The first child stops its queued companion before its body
            // starts. A wrapper would defer stop: {false, true}.
            expect(states == std::vector<bool>{true, true});
            expect(std::get<0>(outcomes).has_value());
            expect(std::get<1>(outcomes).has_value());
        };

        "completion links need no wrapper task slots"_test = [] {
            auto storage = nxtrt::static_deck_task_storage<4>{};
            auto deck = nxtrt::deck{storage};
            auto outcomes = deck.sync_wait([] {
                return nxtrt::settle(
                    std::tuple{
                        value_after_yield(17), value_after_yield(43)});
            });
            expect(std::get<0>(outcomes).value() == 17);
            expect(std::get<1>(outcomes).value() == 43);
            expect(deck.empty());
        };

        "completion links"_group = [] {
            "notify inline without starting or consuming the task"_test =
                [] {
                    auto deck = nxtrt::deck{};
                    auto child = value_after_yield(37);
                    auto calls = 0;
                    auto result = 0;
                    auto link = child.on_completed([&]() noexcept {
                        ++calls;
                        result = child.result();
                    });
                    expect(link.connected());
                    expect(calls == 0 && !child.id());
                    deck.start(child);
                    deck.run_ready();
                    expect(calls == 0);
                    deck.run_ready();
                    expect(calls == 1 && result == 37);
                    expect(!link.connected() && child.done());
                    expect(deck.empty());
                    expect(child.result() == 37);
                    deck.run_until_idle();
                    expect(calls == 1);
                };

            "owns a move-only callback"_test = [] {
                auto deck = nxtrt::deck{};
                auto child = value_after_yield(19);
                auto observed = 0;
                auto link = child.on_completed(
                    [value = std::make_unique<int>(53),
                     &observed]() noexcept { observed = *value; });
                deck.start(child);
                deck.run_until_idle();
                expect(observed == 53 && !link.connected());
                expect(child.result() == 19);
            };

            "completed tasks notify before registration returns"_test = [] {
                auto deck = nxtrt::deck{};
                auto starts = 0;
                auto child = owned_value_after_yield(
                    std::make_unique<int>(29), starts);
                deck.start(child);
                deck.run_until_idle();
                auto calls = 0;
                auto link = child.on_completed([&]() noexcept {
                    ++calls;
                    expect(*child.result() == 29);
                });
                expect(calls == 1 && starts == 1);
                expect(!link.connected());
                expect(*std::move(child).result() == 29);
            };

            "destruction and explicit disconnection detach"_test = [] {
                auto deck = nxtrt::deck{};
                auto child = value_after_yield(19);
                auto calls = 0;
                {
                    auto link =
                        child.on_completed([&]() noexcept { ++calls; });
                    expect(link.connected());
                }
                expect(child.handle().promise().completion == nullptr);
                auto link = child.on_completed([&]() noexcept { ++calls; });
                link.disconnect();
                link.disconnect();
                expect(!link.connected());
                deck.start(child);
                deck.run_until_idle();
                expect(calls == 0 && child.result() == 19);
            };

            "moving links and tasks preserves the registration"_test = [] {
                auto deck = nxtrt::deck{};
                auto first = value_after_yield(17);
                auto second = value_after_yield(23);
                auto calls = 0;
                auto notify = [&]() noexcept { ++calls; };
                auto link = first.on_completed(notify);
                auto links = std::vector<decltype(link)>{};
                links.reserve(1);
                links.push_back(std::move(link));
                links.push_back(
                    second.on_completed(notify)); // Reallocates.
                expect(!link.connected());
                auto moved = std::move(first);
                deck.start(moved);
                deck.start(second);
                deck.run_until_idle();
                expect(calls == 2);
                expect(!links[0].connected() && !links[1].connected());
                expect(moved.result() == 17 && second.result() == 23);
            };

            "destroying the task disconnects a surviving link"_test = [] {
                auto child = value_after_yield(31);
                auto calls = 0;
                auto link = child.on_completed([&]() noexcept { ++calls; });
                child = {};
                expect(!link.connected() && calls == 0);
                link.disconnect();
            };

            "reject empty tasks and duplicate observers"_test = [] {
                auto deck = nxtrt::deck{};
                auto empty = nxtrt::task<int>{};
                auto rejected = false;
                try {
                    auto link = empty.on_completed([]() noexcept {});
                } catch (const nxtrt::runtime_error &) {
                    rejected = true;
                }
                expect(rejected);
                auto child = value_after_yield(7);
                auto first_calls = 0;
                auto second_calls = 0;
                auto first =
                    child.on_completed([&]() noexcept { ++first_calls; });
                rejected = false;
                try {
                    auto second = child.on_completed(
                        [&]() noexcept { ++second_calls; });
                } catch (const nxtrt::runtime_error &) {
                    rejected = true;
                }
                expect(rejected && first.connected());
                deck.start(child);
                deck.run_until_idle();
                expect(first_calls == 1 && second_calls == 0);
            };

            "notification precedes the queued awaiting continuation"_test =
                []() -> nxtrt::task<void> {
                auto child = value_after_yield(41);
                auto parent = nxtrt::current_deck()->current_task_id();
                auto callback_task = nxtrt::task_id{};
                auto link = child.on_completed([&]() noexcept {
                    callback_task =
                        nxtrt::current_deck()->current_task_id();
                    expect(callback_task == child.id());
                });
                auto result = co_await child;
                expect(result == 41);
                expect(callback_task && callback_task != parent);
                expect(nxtrt::current_deck()->current_task_id() == parent);
                expect(!link.connected());
            };
        };
    };
}

} // namespace nxt::test
