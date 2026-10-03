#include "runtime-test.hpp"

namespace nxt::test {

static suite runtime_tests{
    "Runtime", [] {
        "charting"_group = [] {
            "sparkline is a pure width-to-text transform"_test = [] {
                auto values = std::to_array<double>({0.0, 1.0, 2.0});

                auto line = nxtui::chart::sparkline(values, 6);

                expect(line == "    ▄█");
            };

            "sparkline keeps the newest samples when narrow"_test = [] {
                auto values = std::to_array<double>(
                    {0.0, 1.0, 2.0, 3.0, 4.0});

                auto line = nxtui::chart::sparkline(values, 3);

                expect(line == "▄▆█");
            };

            "empty sparkline reserves the requested cells"_test = [] {
                expect(
                    nxtui::chart::sparkline(std::span<const double>{}, 4)
                    == "    ");
            };

            "sparkline can use a fixed value range"_test = [] {
                auto values = std::to_array<double>({0.0, 10.0, 100.0});

                auto line = nxtui::chart::sparkline(
                    values,
                    3,
                    nxtui::chart::value_range{0.0, 100.0});

                expect(line == " ▁█");
            };

            "two-line sparkline gives sixteen vertical steps"_test = [] {
                auto values =
                    std::to_array<double>({0.0, 25.0, 50.0, 75.0, 100.0});

                auto rows = nxtui::chart::sparkline2(
                    values,
                    5,
                    nxtui::chart::value_range{0.0, 100.0});

                expect(rows[0] == "   ▄█");
                expect(rows[1] == " ▄███");
            };

            "progress bar projects fill coverage per cell"_test = [] {
                expect(nxtui::chart::progress_bar(0.625, 4) == "██▌ ");
            };

            "range progress bar projects partial coverage per cell"_test =
                [] {
                expect(nxtui::chart::range_bar(0.25, 0.625, 4) == " █▌ ");
                expect(nxtui::chart::range_bar(0.125, 0.75, 4) == "▐██ ");
            };
        };

        "text flow"_group = [] {
            "wraps paragraphs with markdown list continuation"_test = [] {
                auto lines = nxtui::tui::text_flow::wrap_text(
                    "- hello wide world\n\nnext paragraph",
                    12 * nxtui::ch);

                expect(
                    lines
                    == std::vector<std::string>{
                        "- hello wide",
                        "  world",
                        "",
                        "next",
                        "paragraph"});
            };

            "parses simple inline markdown spans"_test = [] {
                auto spans = nxtui::tui::text_flow::parse_inline_markdown(
                    "a **bold** `code`", nxtui::tui::fg(nxtui::Rgba8::white()));

                expect(spans.size() == std::size_t{4});
                expect(spans[0].text == "a ");
                expect(spans[1].text == "bold");
                expect(has_emphasis(spans[1].style.em, nxtui::Emphasis::bold));
                expect(spans[2].text == " ");
                expect(spans[3].text == "code");
                expect(spans[3].style.bg != nxtui::DEFAULT_COLOR);
            };

            "sanitizes terminal controls while preserving newlines"_test = [] {
                auto text = nxtui::tui::text_flow::sanitize_terminal_text(
                    "a\x1b[31mb\tc\r\nd\x01");

                expect(text == "ab    c\nd");
            };
        };

        "deck"_group = [] {
            "sync_wait returns completed root task values"_test = [] {
                auto deck = nxtrt::deck{};

                expect(deck.sync_wait([]() -> nxtrt::task<int> {
                    co_return 7;
                }) == 7_i);
            };

            "sync_wait factories run inside the deck"_test = [] {
                auto deck = nxtrt::deck{};
                auto had_deck = false;

                deck.sync_wait([&] {
                    had_deck = nxtrt::current_deck() == &deck;
                    return []() -> nxtrt::task<void> { co_return; }();
                });

                expect(had_deck);
            };

            "root_task keeps a root environment for manually pumped tasks"_test = [] {
                auto deck = nxtrt::deck{};
                auto had_deck = false;

                auto root = nxtrt::root_task{
                    deck,
                    [&] {
                        return root_task_probe(had_deck);
                    },
                };

                root.start();
                deck.run_until_idle();

                expect(had_deck);
                expect(std::move(root.inner()).result() == 9_i);
            };

            "resumes tasks awaiting children"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                auto child_body = [&events]() -> nxtrt::task<int> {
                    events.push_back(2);
                    co_await nxtrt::yield();
                    events.push_back(3);
                    co_return 4;
                };

                expect(
                    deck.sync_wait(
                        [&events, child_body]() -> nxtrt::task<int> {
                            events.push_back(1);
                            auto child = child_body();
                            expect(!child.id());

                            auto value = co_await child;

                            expect(static_cast<bool>(child.id()));
                            events.push_back(4);
                            co_return value + 1;
                        })
                    == 5_i);

                expect(events == std::vector<int>{1, 2, 3, 4})
                    << "child/continuation event order changed";
            };

            "borrowed task registries assign compact ids"_test = [] {
                auto storage = nxtrt::static_deck_task_storage<4>{};
                auto deck = nxtrt::deck{storage};

                auto root = nxtrt::root_task{
                    deck,
                    []() -> nxtrt::task<nxtrt::task_id> {
                        auto * deck = nxtrt::current_deck();
                        expect(deck != nullptr);
                        co_return deck->current_task_id();
                    },
                };

                expect(!root.inner().id());
                root.start();
                auto registered = root.inner().id();
                expect(registered.index() == std::uint32_t{1});
                expect(registered.era() == std::uint8_t{1});

                deck.run_ready();

                expect(root.inner().done());
                expect(std::move(root.inner()).result() == registered);
            };

            "borrowed task registries reuse slots with a new era"_test = [] {
                auto storage = nxtrt::static_deck_task_storage<1>{};
                auto deck = nxtrt::deck{storage};

                auto make_task = []() -> nxtrt::task<void> {
                    co_return;
                };

                auto first_id = nxtrt::task_id{};
                {
                    auto first = nxtrt::root_task{deck, make_task};
                    first.start();
                    first_id = first.inner().id();
                    deck.run_ready();
                }

                auto second = nxtrt::root_task{deck, make_task};
                second.start();
                auto second_id = second.inner().id();

                expect(first_id.index() == second_id.index());
                expect(first_id.era() != second_id.era());

                deck.run_ready();
            };

            "borrowed task registries reject overflow"_test = [] {
                auto storage = nxtrt::static_deck_task_storage<1>{};
                auto deck = nxtrt::deck{storage};

                auto parked = nxtrt::root_task{
                    deck,
                    []() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                    },
                };
                auto extra = nxtrt::root_task{
                    deck,
                    []() -> nxtrt::task<void> {
                        co_return;
                    },
                };

                parked.start();

                auto rejected = false;
                try {
                    extra.start();
                } catch (const nxtrt::runtime_error &) {
                    rejected = true;
                }

                expect(rejected);
            };

            "stale ready task ids are ignored"_test = [] {
                auto storage = nxtrt::static_deck_task_storage<1>{};
                auto deck = nxtrt::deck{storage};
                auto events = std::vector<int>{};

                {
                    auto root = nxtrt::root_task{
                        deck,
                        [&] {
                            return record_after_yield(events, 0);
                        },
                    };

                    root.start();
                    deck.run_ready();
                    expect(events == std::vector<int>{1});
                    expect(!deck.empty());
                }
                deck.run_ready();

                expect(events == std::vector<int>{1});
                expect(deck.empty());
            };

            "re-enters yielded tasks through the pump"_test = []() -> nxtrt::task<void> {
                auto out = std::vector<int>{};

                auto child_body = [&out](int tag) -> nxtrt::task<void> {
                    out.push_back(tag * 10 + 1);
                    co_await nxtrt::yield();
                    out.push_back(tag * 10 + 2);
                };

                auto first = child_body(1);
                auto second = child_body(2);

                co_await first;
                co_await second;

                expect(out == std::vector<int>{11, 12, 21, 22})
                    << "yield event order changed";
            };

            "run_ready only plays the initially ready round"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                // Pass state as a coroutine parameter instead of capturing
                // it in a temporary coroutine lambda; captures live in the
                // lambda object, while parameters live in the coroutine
                // frame.
                auto task_body =
                    [](std::vector<int> & events) -> nxtrt::task<void> {
                    events.push_back(1);
                    co_await nxtrt::yield();
                    events.push_back(2);
                };

                auto root = nxtrt::root_task{
                    deck,
                    [&] {
                        return task_body(events);
                    },
                };
                root.start();
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
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                // Same lifetime rule as above: coroutine parameters are
                // frame state.
                auto task_body =
                    [](std::vector<int> & events) -> nxtrt::task<void> {
                    events.push_back(1);
                    co_await nxtrt::yield();
                    events.push_back(2);
                };

                auto root = nxtrt::root_task{
                    deck,
                    [&] {
                        return task_body(events);
                    },
                };
                root.start();
                deck.run_until_idle();

                expect(events == std::vector<int>{1, 2})
                    << "run_until_idle should play all rounds";
                expect(deck.empty())
                    << "deck should be empty after run_until_idle";
            };

            "propagates exceptions through sync_wait"_test = [] {
                auto deck = nxtrt::deck{};

                auto threw = false;
                try {
                    deck.sync_wait([]() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                        throw std::runtime_error{"boom"};
                    });
                } catch (const std::exception &) {
                    threw = true;
                }
                expect(threw);
            };

            "rejects reentrant pump calls"_test = [] {
                auto deck = nxtrt::deck{};

                expect(deck.sync_wait([&deck]() -> nxtrt::task<bool> {
                    try {
                        deck.run_ready();
                    } catch (const std::exception &) {
                        co_return true;
                    }
                    co_return false;
                }));
            };

            "tasks observe their own stop request"_test = [] {
                auto deck = nxtrt::deck{};

                auto root = nxtrt::root_task{
                    deck,
                    []() -> nxtrt::task<bool> {
                        co_return nxtrt::task_stop_requested();
                    },
                };
                root.inner().request_stop();
                root.start();
                deck.run_until_idle();

                expect(std::move(root.inner()).result());
            };

            "ready awaitable resolves without suspending"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto suspends = 0;

                auto root = nxtrt::root_task{
                    deck,
                    [&] {
                        return run_splice_probe(events, suspends, true);
                    },
                };
                root.start();
                deck.run_ready();

                expect(suspends == 0_i)
                    << "ready fast path must not enter await_suspend";
                expect(root.inner().done())
                    << "ready awaitable should finish in one pump round";
                expect(deck.empty())
                    << "ready awaitable should not queue any work";
                expect(events == std::vector<int>{99})
                    << "ready path should skip the delegate task entirely";
                expect(std::move(root.inner()).result() == 42_i);
            };

            "missing awaitable delegates to a spliced task"_test = []() -> nxtrt::task<void> {
                auto events = std::vector<int>{};
                auto suspends = 0;

                auto value = co_await run_splice_probe(events, suspends, false);

                expect(suspends == 1_i)
                    << "miss path should enter await_suspend exactly once";
                expect(value == 42_i);
                expect(events == std::vector<int>{10, 11, 99})
                    << "spliced delegate must finish before the continuation";
            };

            "hope resolves a ready value without a wish"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto suspends = 0;

                auto root = nxtrt::root_task{
                    deck,
                    [&] {
                        return run_hope(events, suspends, true);
                    },
                };
                root.start();
                deck.run_ready();

                expect(suspends == 0_i)
                    << "ready hope must not build a delegate task";
                expect(root.inner().done())
                    << "ready hope should finish in one pump round";
                expect(deck.empty());
                expect(events == std::vector<int>{99});
                expect(std::move(root.inner()).result() == 42_i);
            };

            "hope makes a wish and resumes after it"_test = []() -> nxtrt::task<void> {
                auto events = std::vector<int>{};
                auto suspends = 0;

                auto value = co_await run_hope(events, suspends, false);

                expect(suspends == 1_i)
                    << "miss path should build exactly one delegate task";
                expect(value == 42_i);
                expect(events == std::vector<int>{10, 11, 99})
                    << "delegate wish must finish before the value is read";
            };

            "map transforms a task result"_test = [] {
                auto deck = nxtrt::deck{};
                expect(deck.sync_wait([] {
                    return map_over_task();
                }) == 22_i);
            };

            "map over a ready hope stays synchronous"_test = [] {
                auto deck = nxtrt::deck{};

                auto root = nxtrt::root_task{
                    deck,
                    [] {
                        return map_over_ready_hope();
                    },
                };
                root.start();
                deck.run_ready();

                expect(root.inner().done())
                    << "mapping a ready awaitable must not add a suspension";
                expect(deck.empty());
                expect(std::move(root.inner()).result() == 42_i);
            };

            "map transforms a wish result over one round-trip"_test = [] {
                auto wand = manual_wand{};
                auto deck = nxtrt::deck{&wand};

                auto root = nxtrt::root_task{
                    deck,
                    [] {
                        return map_over_manual_wish(55);
                    },
                };
                root.start();
                deck.run_ready();

                expect(!root.inner().done())
                    << "mapped wish should park, not complete synchronously";
                expect(wand.prepared == std::vector<nxtrt::coin_t>{55})
                    << "map must forward the wish's preparation to the wand";
                expect(wand.parked.size() == std::size_t{1})
                    << "map must forward the single suspension";

                wand.fulfill(deck, 55);
                deck.run_ready();

                expect(root.inner().done());
                expect(std::move(root.inner()).result() == 7_i)
                    << "transform must run in the resume after fulfillment";
            };

            "then transforms task values"_test = []() -> nxtrt::task<void> {
                auto result = co_await nxtrt::then(value_after_yield(20), [](int value) {
                    return value + 1;
                });

                expect(result == 21_i);
            };

            "let_value chains task values"_test = []() -> nxtrt::task<void> {
                auto result = co_await nxtrt::let_value(
                    value_after_yield(20),
                    [](int value) {
                    return value_after_yield(value + 2);
                });

                expect(result == 22_i);
            };

            "finally runs shielded cleanup before returning values"_test = []() -> nxtrt::task<void> {
                auto events = std::vector<int>{};

                auto result = co_await nxtrt::finally(
                value_after_yield(7),
                [&]() {
                    return record_after_yield(events, 9);
                });

                expect(result == 7_i);
                expect(events == std::vector<int>{91, 92});
            };

            "finally runs cleanup after body failure"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    (void)deck.sync_wait([&] {
                        return nxtrt::finally(
                            throw_int_after_yield(),
                            [&]() {
                                return record_after_yield(events, 8);
                            });
                    });
                } catch (const std::exception &) {
                    threw = true;
                }

                expect(threw);
                expect(events == std::vector<int>{81, 82});
            };

            "finally groups body and cleanup failures"_test = [] {
                auto deck = nxtrt::deck{};
                auto grouped = false;

                try {
                    (void)deck.sync_wait([] {
                        return nxtrt::finally(
                            throw_int_after_yield(),
                            []() -> nxtrt::task<void> {
                                co_await nxtrt::yield();
                                throw nxtrt::runtime_error{"cleanup boom"};
                            });
                    });
                } catch (const nxtrt::exception_group & group) {
                    grouped = true;
                    expect(group.exceptions().size() == std::size_t{2});
                }

                expect(grouped);
            };

            "task adaptors flow through then and let_value"_test = []() -> nxtrt::task<void> {
                auto events = std::vector<int>{};

                auto result = co_await (
                    value_after_yield(10)
                    | nxtrt::then([](int value) {
                        return value * 2;
                    })
                    | nxtrt::let_value([](int value) {
                        return value_after_yield(value + 5);
                    })
                    | nxtrt::finally([&]() {
                        return record_after_yield(events, 6);
                    }));

                expect(result == 25_i);
                expect(events == std::vector<int>{61, 62});
            };

            "for_each_task awaits lazy ranges of tasks"_test = []() -> nxtrt::task<void> {
                auto values = std::array{1, 2, 3};
                auto events = std::vector<int>{};

                co_await nxtrt::for_each_task(
                    values | std::views::transform(
                        [&](int value) {
                            return record_after_yield(events, value);
                        }));

                expect(events == std::vector<int>{11, 12, 21, 22, 31, 32});
            };

            "when_all_range awaits lazy ranges concurrently"_test = []() -> nxtrt::task<void> {
                auto values = std::array{1, 2, 3};

                std::vector<int> result = co_await nxtrt::when_all_range(
                    values | std::views::transform(
                        [](int value) {
                            return value_after_yield(value * 10);
                        }));

                expect(result == std::vector<int>{10, 20, 30});
            };

            "wait_any_range awaits lazy ranges concurrently"_test = []() -> nxtrt::task<void> {
                auto values = std::array{5, 6};
                auto events = std::vector<int>{};

                int result = co_await nxtrt::wait_any_range(
                    values | std::views::transform(
                        [&](int value) {
                            if (value == 5)
                                return value_after_yield(value);
                            return value_after_two_yields_or_stop(
                                events,
                                value);
                        }));

                expect(result == 5_i);
                expect(events == std::vector<int>{6});
            };
        };

        "environment"_group = [] {

            "empty optional refs throw on access"_test = [] {
                struct probe
                {
                    int value = 0;
                };

                auto deref_threw = false;
                try {
                    auto ref = nxtrt::optional_ref<const probe>{};
                    (void)*ref;
                } catch (const nxtrt::runtime_error &) {
                    deref_threw = true;
                }

                auto arrow_threw = false;
                try {
                    auto ref = nxtrt::optional_ref<const probe>{};
                    (void)ref->value;
                } catch (const nxtrt::runtime_error &) {
                    arrow_threw = true;
                }

                auto get_threw = false;
                try {
                    auto ref = nxtrt::optional_ref<const probe>{};
                    (void)ref.get();
                } catch (const nxtrt::runtime_error &) {
                    get_threw = true;
                }

                expect(deref_threw);
                expect(arrow_threw);
                expect(get_threw);
            };

            "survives nested task awaits"_test = []() -> nxtrt::task<void> {
                int result = co_await nxtrt::with_env<ambient_int_key>(
                    41, [] { return read_ambient_int_after_yield(); });

                expect(result == 41_i);
            };

            "restores outer env values"_test = []() -> nxtrt::task<void> {
                int result = co_await nxtrt::with_env<ambient_int_key>(
                    10, []() -> nxtrt::task<int> {
                        auto before = co_await read_ambient_int();
                        auto inside = co_await nxtrt::with_env<
                            ambient_int_key>(20, [] {
                            return read_ambient_int_after_yield();
                        });
                        auto after = co_await read_ambient_int();
                        co_return before * 100 + inside * 10 + after;
                    });

                expect(result == 1210_i);
            };

            "group jobs see the env bound around the group"_test = []() -> nxtrt::task<void> {
                auto values = co_await nxtrt::with_env<ambient_int_key>(
                    99, [] {
                        return nxtrt::when_all(
                            std::tuple{read_ambient_int_after_yield()});
                    });
                int result = std::get<0>(values);

                expect(result == 99_i);
            };

            "trace context is inherited by group jobs"_test = []() -> nxtrt::task<void> {
                auto trace = std::make_shared<nxtrt::trace_context>();
                auto root = trace->start_span("root");

                auto traced_child =
                    [](std::string name) -> nxtrt::task<void> {
                    auto trace = nxtrt::current_trace_context();
                    auto span = trace->start_span(
                        std::move(name),
                        nxtrt::current_trace_span_id());
                    co_await nxtrt::yield();
                    span.finish("ok");
                };

                co_await nxtrt::with_env<nxtrt::trace_context_key>(
                    trace, [&]() -> nxtrt::task<void> {
                        co_await nxtrt::with_env<
                            nxtrt::trace_current_span_key>(
                            root.span_id(), [&]() -> nxtrt::task<void> {
                                (void)co_await nxtrt::when_all(
                                    traced_child("child-a"),
                                    traced_child("child-b"));
                            });
                    });

                root.finish("ok");
                auto children = trace->children(root.span_id());
                expect(children.size() == std::size_t{2});
                expect(children[0].name == "child-a"sv);
                expect(children[1].name == "child-b"sv);
                expect(children[0].status == "ok"sv);
                expect(children[1].status == "ok"sv);
            };

            "with trace span scopes task bodies"_test = []() -> nxtrt::task<void> {
                auto trace = std::make_shared<nxtrt::trace_context>();
                auto root = trace->start_span("root");

                int result = co_await nxtrt::with_env<
                    nxtrt::trace_context_key>(
                    trace, [&]() -> nxtrt::task<int> {
                    co_return co_await nxtrt::with_env<
                        nxtrt::trace_current_span_key>(
                        root.span_id(), [&]() -> nxtrt::task<int> {
                        co_return co_await nxtrt::with_trace_span(
                            "child",
                            []() -> nxtrt::task<int> {
                            co_await nxtrt::yield();
                            co_return 42;
                        });
                    });
                });

                root.finish("ok");
                auto children = trace->children(root.span_id());
                expect(result == 42_i);
                expect(children.size() == std::size_t{1});
                expect(children[0].name == "child"sv);
                expect(children[0].status == "ok"sv);
            };
        };

        "terminal app"_group = [] {
            "keeps the alternate screen opt-in"_test = [] {
                auto options = nxtrt::terminal_app_options{};
                expect(!options.alternate_screen);
            };
        };

        declare_runtime_group_tests();
        declare_runtime_buffer_tests();
        declare_runtime_io_tests();
    }};

} // namespace nxt::test
