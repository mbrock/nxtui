#include <nxt/sparkline.hpp>
#include <nxt/tui_text.hpp>
#include <nxt/rt/buffers.hpp>
#include <nxt/rt/channel.hpp>
#include <nxt/rt/event.hpp>
#include <nxt/rt/http.hpp>
#include <nxt/rt/kqueue_wand.hpp>
#include <nxt/rt/task.hpp>
#include <nxt/rt/terminal_app.hpp>
#include <nxt/rt/ui_runtime.hpp>
#include <nxtai/tool_batch.hpp>

#include "test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
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

protected:
    nxt::rt::wait_token prepare_wish(
        nxt::rt::deck &,
        nxt::rt::detail::promise_base &,
        nxt::rt::detail::prepared_wish packet) override
    {
        auto * wish = std::get_if<nxt::rt::op::manual>(&packet.wish);
        if (wish == nullptr)
            throw std::runtime_error{
                "manual_wand only implements manual wishes"};

        prepared.push_back(wish->token);
        states.push_back(
            std::static_pointer_cast<nxt::rt::wait_state<void>>(
                packet.state));
        return wish->token;
    }

public:
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

struct chunking_string_sink final : nxt::rt::byte_sink
{
    explicit chunking_string_sink(std::size_t limit)
        : limit(limit)
    {}

    nxt::rt::task<std::size_t>
    write_some(std::span<const std::byte> src) override
    {
        auto n = std::min(limit, src.size());
        text += nxt::rt::as_string_view(src.first(n));
        co_return n;
    }

    std::string text;
    std::size_t limit = 1;
};

struct shared_string_sink final : nxt::rt::byte_sink
{
    explicit shared_string_sink(
        std::shared_ptr<std::string> text,
        std::size_t limit = 64)
        : text(std::move(text))
        , limit(limit)
    {}

    nxt::rt::task<std::size_t>
    write_some(std::span<const std::byte> src) override
    {
        auto n = std::min(limit, src.size());
        *text += nxt::rt::as_string_view(src.first(n));
        co_return n;
    }

    std::shared_ptr<std::string> text;
    std::size_t limit = 1;
};

struct ng_echo_tool
{
    static constexpr std::string_view name = "ng_echo";
    static constexpr std::string_view description = "Echo text.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string text;

        struct glaze_json_schema
        {
            glz::schema text{.description = "Text to echo."};
        };
    };

    nxt::rt::task<nxt::ai::tools::tool_result> run(parameters args) const
    {
        co_await nxt::rt::yield();
        co_return nxt::ai::tools::tool_result{.output = std::move(args.text)};
    }
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

nxt::rt::task<void>
record_next_channel_value(
    nxt::rt::channel<int> & events,
    std::vector<int> & out)
{
    auto value = co_await events.next();
    if (value)
        out.push_back(*value);
}

nxt::rt::task<void>
record_closed_channel(nxt::rt::channel<int> & events, bool & finished)
{
    auto value = co_await events.next();
    expect(!value);
    finished = true;
}

nxt::rt::task<void>
record_after_event(
    nxt::rt::event & ready,
    std::vector<int> & out,
    int value)
{
    co_await ready;
    out.push_back(value);
}

nxt::rt::task<void> record_current_zone(
    std::vector<nxt::rt::task_zone *> & zones)
{
    co_await nxt::rt::yield();
    zones.push_back(nxt::rt::current_zone());
}

nxt::rt::task<bool> read_task_stop_after_yield()
{
    co_await nxt::rt::yield();
    co_return nxt::rt::task_stop_requested();
}

nxt::rt::task<bool> shielded_child_stop_state()
{
    co_return co_await nxt::rt::shield(read_task_stop_after_yield());
}

nxt::rt::task<void> await_manual_token(nxt::rt::wait_token token)
{
    co_await nxt::rt::op::manual{.token = token};
}

nxt::rt::task<void> shielded_manual_token(nxt::rt::wait_token token)
{
    co_await nxt::rt::shield(await_manual_token(token));
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
        "charting"_test = [] {
            "sparkline is a pure width-to-text transform"_test = [] {
                auto values = std::to_array<double>({0.0, 1.0, 2.0});

                auto line = nxt::chart::sparkline(values, 6);

                expect(line == "    ▄█");
            };

            "sparkline keeps the newest samples when narrow"_test = [] {
                auto values = std::to_array<double>(
                    {0.0, 1.0, 2.0, 3.0, 4.0});

                auto line = nxt::chart::sparkline(values, 3);

                expect(line == "▄▆█");
            };

            "empty sparkline reserves the requested cells"_test = [] {
                expect(
                    nxt::chart::sparkline(std::span<const double>{}, 4)
                    == "    ");
            };

            "sparkline can use a fixed value range"_test = [] {
                auto values = std::to_array<double>({0.0, 10.0, 100.0});

                auto line = nxt::chart::sparkline(
                    values,
                    3,
                    nxt::chart::value_range{0.0, 100.0});

                expect(line == " ▁█");
            };

            "two-line sparkline gives sixteen vertical steps"_test = [] {
                auto values =
                    std::to_array<double>({0.0, 25.0, 50.0, 75.0, 100.0});

                auto rows = nxt::chart::sparkline2(
                    values,
                    5,
                    nxt::chart::value_range{0.0, 100.0});

                expect(rows[0] == "   ▄█");
                expect(rows[1] == " ▄███");
            };

            "progress bar projects fill coverage per cell"_test = [] {
                expect(nxt::chart::progress_bar(0.625, 4) == "██▌ ");
            };

            "range progress bar projects partial coverage per cell"_test =
                [] {
                expect(nxt::chart::range_bar(0.25, 0.625, 4) == " █▌ ");
                expect(nxt::chart::range_bar(0.125, 0.75, 4) == "▐██ ");
            };
        };

        "text flow"_test = [] {
            "wraps paragraphs with markdown list continuation"_test = [] {
                auto lines = nxt::tui::text_flow::wrap_text(
                    "- hello wide world\n\nnext paragraph",
                    12 * nxt::ch);

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
                auto spans = nxt::tui::text_flow::parse_inline_markdown(
                    "a **bold** `code`", nxt::tui::fg(nxt::Rgba8::white()));

                expect(spans.size() == std::size_t{4});
                expect(spans[0].text == "a ");
                expect(spans[1].text == "bold");
                expect(has_emphasis(spans[1].style.em, nxt::Emphasis::bold));
                expect(spans[2].text == " ");
                expect(spans[3].text == "code");
                expect(spans[3].style.bg != nxt::DEFAULT_COLOR);
            };

            "sanitizes terminal controls while preserving newlines"_test = [] {
                auto text = nxt::tui::text_flow::sanitize_terminal_text(
                    "a\x1b[31mb\tc\r\nd\x01");

                expect(text == "ab    c\nd");
            };
        };

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

            "then transforms task values"_test = [] {
                auto deck = nxt::rt::deck{};

                auto result = deck.sync_wait(
                    nxt::rt::then(value_after_yield(20), [](int value) {
                        return value + 1;
                    }));

                expect(result == 21_i);
            };

            "let_value chains task values"_test = [] {
                auto deck = nxt::rt::deck{};

                auto result = deck.sync_wait(
                    nxt::rt::let_value(value_after_yield(20), [](int value) {
                        return value_after_yield(value + 2);
                    }));

                expect(result == 22_i);
            };

            "finally runs shielded cleanup before returning values"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                auto result = deck.sync_wait(nxt::rt::finally(
                    value_after_yield(7),
                    [&]() {
                        return record_after_yield(events, 9);
                    }));

                expect(result == 7_i);
                expect(events == std::vector<int>{91, 92});
            };

            "finally runs cleanup after body failure"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    (void)deck.sync_wait(nxt::rt::finally(
                        throw_int_after_yield(),
                        [&]() {
                            return record_after_yield(events, 8);
                        }));
                } catch (const std::exception &) {
                    threw = true;
                }

                expect(threw);
                expect(events == std::vector<int>{81, 82});
            };

            "finally groups body and cleanup failures"_test = [] {
                auto deck = nxt::rt::deck{};
                auto grouped = false;

                try {
                    (void)deck.sync_wait(nxt::rt::finally(
                        throw_int_after_yield(),
                        []() -> nxt::rt::task<void> {
                            co_await nxt::rt::yield();
                            throw nxt::rt::runtime_error{"cleanup boom"};
                        }));
                } catch (const nxt::rt::exception_group & group) {
                    grouped = true;
                    expect(group.exceptions().size() == std::size_t{2});
                }

                expect(grouped);
            };

            "task adaptors flow through then and let_value"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = std::vector<int>{};

                auto result = deck.sync_wait(
                    value_after_yield(10)
                    | nxt::rt::then([](int value) {
                        return value * 2;
                    })
                    | nxt::rt::let_value([](int value) {
                        return value_after_yield(value + 5);
                    })
                    | nxt::rt::finally([&]() {
                        return record_after_yield(events, 6);
                    }));

                expect(result == 25_i);
                expect(events == std::vector<int>{61, 62});
            };

            "for_each_task awaits lazy ranges of tasks"_test = [] {
                auto deck = nxt::rt::deck{};
                auto values = std::array{1, 2, 3};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::for_each_task(
                        values | std::views::transform(
                            [&](int value) {
                                return record_after_yield(events, value);
                            }));
                });

                expect(events == std::vector<int>{11, 12, 21, 22, 31, 32});
            };

            "when_all_range awaits lazy ranges concurrently"_test = [] {
                auto deck = nxt::rt::deck{};
                auto values = std::array{1, 2, 3};

                auto result = deck.sync_wait([&]() -> nxt::rt::task<std::vector<int>> {
                    co_return co_await nxt::rt::when_all_range(
                        values | std::views::transform(
                            [](int value) {
                                return value_after_yield(value * 10);
                            }));
                });

                expect(result == std::vector<int>{10, 20, 30});
            };

            "wait_any_range awaits lazy ranges concurrently"_test = [] {
                auto deck = nxt::rt::deck{};
                auto values = std::array{5, 6};
                auto events = std::vector<int>{};

                auto result = deck.sync_wait([&]() -> nxt::rt::task<int> {
                    co_return co_await nxt::rt::wait_any_range(
                        values | std::views::transform(
                            [&](int value) {
                                if (value == 5)
                                    return value_after_yield(value);
                                return value_after_two_yields_or_stop(
                                    events,
                                    value);
                            }));
                });

                expect(result == 5_i);
                expect(events == std::vector<int>{6});
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

            "trace context is inherited by forked tasks"_test = [&] {
                auto trace = std::make_shared<nxt::rt::trace_context>();
                auto root = trace->start_span("root");

                auto traced_child =
                    [](std::string name) -> nxt::rt::task<void> {
                    auto trace = nxt::rt::current_trace_context();
                    auto span = trace->start_span(
                        std::move(name),
                        nxt::rt::current_trace_span_id());
                    co_await nxt::rt::yield();
                    span.finish("ok");
                };

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_env<nxt::rt::trace_context_key>(
                        trace, [&]() -> nxt::rt::task<void> {
                        co_await nxt::rt::with_env<
                            nxt::rt::trace_current_span_key>(
                            root.span_id(), [&]() -> nxt::rt::task<void> {
                            co_await nxt::rt::with_zone(
                                [&]() -> nxt::rt::task<void> {
                                nxt::rt::fork(traced_child("child-a"));
                                nxt::rt::fork(traced_child("child-b"));
                                co_return;
                            });
                        });
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

            "with trace span scopes task bodies"_test = [&] {
                auto trace = std::make_shared<nxt::rt::trace_context>();
                auto root = trace->start_span("root");

                auto result = deck.sync_wait([&]() -> nxt::rt::task<int> {
                    co_return co_await nxt::rt::with_env<
                        nxt::rt::trace_context_key>(
                        trace, [&]() -> nxt::rt::task<int> {
                        co_return co_await nxt::rt::with_env<
                            nxt::rt::trace_current_span_key>(
                            root.span_id(), [&]() -> nxt::rt::task<int> {
                            co_return co_await nxt::rt::with_trace_span(
                                "child",
                                []() -> nxt::rt::task<int> {
                                co_await nxt::rt::yield();
                                co_return 42;
                            });
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

        "terminal app"_test = [] {
            "keeps the alternate screen opt-in"_test = [] {
                auto options = nxt::rt::terminal_app_options{};
                expect(!options.alternate_screen);
            };
        };

        "ui scopes"_test = [] {
            "child scopes expose independently drawable surfaces"_test = [] {
                auto runtime = nxt::rt::ui_runtime{
                    {.render = false,
                     .fallback_size = {16 * nxt::ch, 4 * nxt::ln}}};
                auto root = nxt::rt::ui_scope{runtime};
                auto child = root.child();

                root.draw(nxt::tui::row(child.surface(), nxt::tui::text("!")));
                child.draw(nxt::tui::text("hi"));

                expect(child.surface().width_hint().min == 2 * nxt::ch);
                expect(root.surface().width_hint().min == 3 * nxt::ch);
            };

            "spawned child scopes clear their surface on exit"_test = [] {
                auto runtime = nxt::rt::ui_runtime{
                    {.render = false,
                     .fallback_size = {16 * nxt::ch, 4 * nxt::ln}}};
                auto root = nxt::rt::ui_scope{runtime};
                auto captured =
                    nxt::tui::Slot<nxt::tui::AnyLayout>{nxt::tui::AnyLayout{}};
                auto ran = false;
                auto deck = nxt::rt::deck{};

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
                        auto child = root.spawn(
                            [&](nxt::rt::ui_scope child_scope)
                                -> nxt::rt::task<void> {
                                child_scope.draw(nxt::tui::text("busy"));
                                ran = true;
                                co_return;
                            });
                        captured = child.surface();
                        root.draw(captured);
                        co_return;
                    });
                });

                expect(ran);
                expect(captured.width_hint().min == 0 * nxt::ch);
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

            "shielded child tasks do not inherit parent stop"_test = [] {
                auto deck = nxt::rt::deck{};
                auto task = shielded_child_stop_state();

                deck.start(task);
                task.request_stop();
                deck.run_until_idle();

                expect(task.done());
                expect(!std::move(task).result());
            };

            "shielded child wishes are not cancelled by parent stop"_test = [] {
                auto wand = manual_wand{};
                auto deck = nxt::rt::deck{&wand};
                auto task = shielded_manual_token(99);

                deck.start(task);
                task.request_stop();
                deck.run_until_idle();

                expect(wand.prepared == std::vector<nxt::rt::wait_token>{99});
                expect(wand.cancelled.empty());
                expect(wand.parked.size() == std::size_t{1});

                wand.fulfill(deck, 99);
                deck.run_until_idle();

                expect(task.done());
                std::move(task).result();
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

        "tool batches"_test = [] {
            "parse calls and return function_call_output items in order"_test = [] {
                auto deck = nxt::rt::deck{};
                auto calls = std::vector<nxt::ai::tools::function_call>{
                    *nxt::ai::tools::function_call_from_item(
                        nxt::ai::openai::raw_json{
                            R"({"id":"fc_1","type":"function_call","call_id":"call_1","name":"ng_echo","arguments":"{\"text\":\"one\"}"})"}),
                    *nxt::ai::tools::function_call_from_item(
                        nxt::ai::openai::raw_json{
                            R"({"id":"fc_2","type":"function_call","call_id":"call_2","name":"ng_echo","arguments":"{\"text\":\"two\"}"})"}),
                };
                auto tools = nxt::ai::tools::tool_set{ng_echo_tool{}};

                auto results = deck.sync_wait(
                    nxt::ai::tools::run_function_tool_batch(
                        tools,
                        std::move(calls)));

                expect(results.size() == 2_ul);
                expect(results[0].call.call_id == "call_1");
                expect(results[0].result.output == "one");
                expect(results[1].call.call_id == "call_2");
                expect(results[1].result.output == "two");
                expect(
                    results[0].output_item.str.find("function_call_output")
                    != std::string::npos);
                expect(
                    results[0].output_item.str.find(
                        R"("output":"{\"failed\":false,\"output\":\"one\"}")")
                    != std::string::npos);
            };

            "unknown tools become failed batch results"_test = [] {
                auto deck = nxt::rt::deck{};
                auto tools = nxt::ai::tools::tool_set{ng_echo_tool{}};
                auto calls = std::vector<nxt::ai::tools::function_call>{
                    nxt::ai::tools::function_call{
                        .call_id = "call_missing",
                        .name = "missing",
                        .arguments = "{}",
                    },
                };

                auto results = deck.sync_wait(
                    nxt::ai::tools::run_function_tool_batch(
                        tools,
                        std::move(calls)));

                expect(results.size() == 1_ul);
                expect(results[0].result.failed);
                expect(results[0].result.output == "unknown tool");
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
                    co_await nxt::rt::op::manual{.token = 42};
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
                    co_await nxt::rt::op::manual{.token = 7};
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
                    co_await nxt::rt::op::manual{.token = 99};
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

            "write_all drains borrowed bytes into sinks"_test = [] {
                auto deck = nxt::rt::deck{};
                auto sink = chunking_string_sink{2};

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await nxt::rt::write_all(
                        sink,
                        std::string{"abcdef"});
                });

                expect(sink.text == "abcdef");
            };

            "task_byte_source reads through a task callable"_test = [] {
                "from read results"_test = [] {
                    auto deck = nxt::rt::deck{};
                    auto read = [](std::span<std::byte> dst)
                        -> nxt::rt::task<nxt::rt::read_result> {
                        auto text = std::string_view{"xy"};
                        std::memcpy(dst.data(), text.data(), text.size());
                        co_return nxt::rt::read_result{
                            .bytes = text.size(),
                            .eof = true,
                        };
                    };
                    auto source = nxt::rt::task_byte_source{read};
                    auto storage = std::array<std::byte, 4>{};

                    auto result = deck.sync_wait(
                        [&]() -> nxt::rt::task<std::string> {
                        auto read = co_await source.read_some(storage);
                        co_return std::string{
                            nxt::rt::as_string_view(
                                std::span{storage}.first(read.bytes))};
                    });

                    expect(result == "xy");
                };

                "from byte counts"_test = [] {
                    auto deck = nxt::rt::deck{};
                    auto read = [](std::span<std::byte> dst)
                        -> nxt::rt::task<std::size_t> {
                        auto text = std::string_view{"xy"};
                        std::memcpy(dst.data(), text.data(), text.size());
                        co_return text.size();
                    };
                    auto source = nxt::rt::task_byte_source{read};
                    auto storage = std::array<std::byte, 4>{};

                    auto result = deck.sync_wait(
                        [&]() -> nxt::rt::task<nxt::rt::read_result> {
                        co_return co_await source.read_some(storage);
                    });

                    expect(result.bytes == std::size_t{2});
                    expect(!result.eof);
                };
            };

            "byte_reader peeks and takes copied structs"_test = [] {
                struct pair
                {
                    unsigned char a = 0;
                    unsigned char b = 0;
                };

                auto deck = nxt::rt::deck{};
                auto chunks = std::array{"abcd"sv};
                auto source = nxt::rt::string_source{std::span{chunks}};
                auto storage = std::array<std::byte, 4>{};
                auto reader =
                    nxt::rt::byte_reader{source, std::span{storage}};

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    auto first = co_await reader.peek_struct<pair>();
                    expect(first.a == static_cast<unsigned char>('a'));
                    expect(first.b == static_cast<unsigned char>('b'));
                    expect(reader.buffered_size() == std::size_t{4});

                    auto second = co_await reader.take_struct<pair>();
                    expect(second.has_value());
                    expect(second->a == static_cast<unsigned char>('a'));
                    expect(second->b == static_cast<unsigned char>('b'));
                    expect(reader.buffered_size() == std::size_t{2});
                });
            };

            "byte_reader returns nullopt when taking structs at eof"_test = [] {
                struct pair
                {
                    unsigned char a = 0;
                    unsigned char b = 0;
                };

                auto deck = nxt::rt::deck{};
                auto chunks = std::array{""sv};
                auto source = nxt::rt::string_source{std::span{chunks}};
                auto storage = std::array<std::byte, 4>{};
                auto reader =
                    nxt::rt::byte_reader{source, std::span{storage}};

                auto result = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::optional<pair>> {
                    co_return co_await reader.take_struct<pair>();
                });

                expect(!result);
            };

            "byte_reader does not treat empty reads as struct eof"_test = [] {
                struct pair
                {
                    unsigned char a = 0;
                    unsigned char b = 0;
                };

                auto deck = nxt::rt::deck{};
                auto source = empty_then_string_source{};
                auto storage = std::array<std::byte, 8>{};
                auto reader =
                    nxt::rt::byte_reader{source, std::span{storage}};

                auto result = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::optional<pair>> {
                    co_return co_await reader.take_struct<pair>();
                });

                expect(result.has_value());
                expect(result->a == static_cast<unsigned char>('a'));
                expect(result->b == static_cast<unsigned char>('b'));
            };

            "byte_reader takes borrowed string views"_test = [] {
                auto deck = nxt::rt::deck{};
                auto chunks = std::array{"abcd"sv};
                auto source = nxt::rt::string_source{std::span{chunks}};
                auto storage = std::array<std::byte, 4>{};
                auto reader =
                    nxt::rt::byte_reader{source, std::span{storage}};

                auto result = deck.sync_wait([&]() -> nxt::rt::task<std::string> {
                    auto view = co_await reader.take_string_view(3);
                    co_return std::string{view};
                });

                expect(result == "abc");
                expect(reader.buffered_size() == std::size_t{1});
            };

            "BYTE WRITER"_test = [] {
                "with borrowed storage"_test = [] {
                    "buffers bytes until flush"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto storage = std::array<std::byte, 4>{};
                        auto writer =
                            nxt::rt::byte_writer{sink, std::span{storage}};

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await writer.write(std::string{"ab"});
                            expect(sink.text.empty());
                            co_await writer.write(std::string{"cd"});
                            expect(sink.text.empty());
                            co_await writer.write(std::string{"e"});
                            expect(sink.text == "abcd");
                            co_await writer.flush();
                        });

                        expect(sink.text == "abcde");
                    };
                };

                "with owned storage"_test = [] {
                    "buffers bytes until flush"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto writer =
                            nxt::rt::byte_writer{sink, std::size_t{4}};

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await writer.write(std::string{"ab"});
                            expect(sink.text.empty());
                            co_await writer.write(std::string{"cd"});
                            expect(sink.text.empty());
                            co_await writer.flush();
                        });

                        expect(sink.text == "abcd");
                    };

                    "writes ranges of text chunks"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto writer =
                            nxt::rt::byte_writer{sink, std::size_t{4}};
                        auto chunks =
                            std::vector<std::string>{"ab", "cd", "e"};

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await writer.write(chunks);
                            expect(sink.text == "abcd");
                            co_await writer.flush();
                        });

                        expect(sink.text == "abcde");
                    };

                    "writes and flushes text chunks"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto writer =
                            nxt::rt::byte_writer{sink, std::size_t{8}};
                        auto chunks =
                            std::vector<std::string>{"ab", "cd", "e"};

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await writer.write_all(chunks);
                        });

                        expect(sink.text == "abcde");
                        expect(writer.buffered_size() == std::size_t{0});
                    };

                    "writes lazy ranges of text chunks"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto writer =
                            nxt::rt::byte_writer{sink, std::size_t{8}};
                        auto numbers = std::views::iota(1, 4);
                        auto chunks = numbers
                            | std::views::transform([](int n) {
                                return std::to_string(n);
                            });

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await writer.write(chunks);
                            co_await writer.flush();
                        });

                        expect(sink.text == "123");
                    };

                    "writes ranges of byte spans"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto writer =
                            nxt::rt::byte_writer{sink, std::size_t{4}};
                        auto chunks = std::array{
                            nxt::rt::as_bytes("ab"sv),
                            nxt::rt::as_bytes("cd"sv),
                            nxt::rt::as_bytes("ef"sv),
                        };

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await writer.write(chunks);
                            co_await writer.flush();
                        });

                        expect(sink.text == "abcdef");
                    };

                    "writes and flushes byte spans"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto writer =
                            nxt::rt::byte_writer{sink, std::size_t{8}};
                        auto chunks = std::array{
                            nxt::rt::as_bytes("ab"sv),
                            nxt::rt::as_bytes("cd"sv),
                            nxt::rt::as_bytes("ef"sv),
                        };

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await writer.write_all(chunks);
                        });

                        expect(sink.text == "abcdef");
                        expect(writer.buffered_size() == std::size_t{0});
                    };

                    "free write_all borrows lvalue sinks"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto chunks =
                            std::vector<std::string>{"ab", "cd", "e"};

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await nxt::rt::write_all(
                                sink,
                                chunks,
                                std::size_t{4});
                        });

                        expect(sink.text == "abcde");
                    };

                    "free write_all owns rvalue sinks"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto text = std::make_shared<std::string>();
                        auto chunks =
                            std::vector<std::string>{"ab", "cd", "e"};

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await nxt::rt::write_all(
                                shared_string_sink{text},
                                chunks,
                                std::size_t{4});
                        });

                        expect(*text == "abcde");
                    };
                };

                "with owned sink and storage"_test = [] {
                    "buffers bytes until flush"_test = [] {
                        auto deck = nxt::rt::deck{};
                        auto text = std::make_shared<std::string>();
                        auto writer = nxt::rt::byte_writer{
                            shared_string_sink{text},
                            std::size_t{4},
                        };

                        deck.sync_wait([&]() -> nxt::rt::task<void> {
                            co_await writer.write(std::string{"abc"});
                            expect(text->empty());
                            co_await writer.write(std::string{"de"});
                            expect(*text == "abcd");
                            co_await writer.flush();
                        });

                        expect(*text == "abcde");
                    };
                };
            };
        };

        "channels"_test = [] {
            "buffer values until consumed"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = nxt::rt::channel<int>{};

                expect(deck.sync_wait(events.publish(1)));
                expect(deck.sync_wait(events.publish(2)));

                auto values = deck.sync_wait([&]() -> nxt::rt::task<
                    std::vector<int>> {
                    auto out = std::vector<int>{};
                    out.push_back(*(co_await events.next()));
                    out.push_back(*(co_await events.next()));
                    co_return out;
                });

                expect(values == std::vector<int>{1, 2});
            };

            "resumes a waiting consumer when a value is published"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = nxt::rt::channel<int>{};
                auto seen = std::vector<int>{};

                auto consumer = record_next_channel_value(events, seen);

                deck.start(consumer);
                deck.run_until_idle();

                expect(seen.empty());
                expect(!consumer.done());

                expect(deck.sync_wait(events.publish(7)));
                deck.run_until_idle();

                expect(seen == std::vector<int>{7});
                expect(consumer.done());
            };

            "close rejects publishers and drains consumers"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = nxt::rt::channel<int>{};

                expect(deck.sync_wait(events.publish(1)));
                events.close();

                auto first = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::optional<int>> {
                    co_return co_await events.next();
                });
                auto second = deck.sync_wait(
                    [&]() -> nxt::rt::task<std::optional<int>> {
                    co_return co_await events.next();
                });

                expect(first && *first == 1_i);
                expect(!second);
                expect(!deck.sync_wait(events.publish(2)));
            };

            "cancel requests stop and wakes pending consumers"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = nxt::rt::channel<int>{};
                auto finished = false;

                auto consumer = record_closed_channel(events, finished);

                deck.start(consumer);
                deck.run_until_idle();

                events.cancel();
                deck.run_until_idle();

                expect(events.stop_requested());
                expect(finished);
                expect(consumer.done());
                expect(!deck.sync_wait(events.publish(1)));
            };

            "try_pop drains buffered values without awaiting"_test = [] {
                auto deck = nxt::rt::deck{};
                auto events = nxt::rt::channel<int>{};

                expect(deck.sync_wait(events.push(3)));
                auto value = events.try_pop();
                expect(value && *value == 3_i);
                expect(!events.try_pop());
            };
        };

        "events"_test = [] {
            "set wakes all waiting tasks"_test = [] {
                auto deck = nxt::rt::deck{};
                auto ready = nxt::rt::event{};
                auto values = std::vector<int>{};

                auto first = record_after_event(ready, values, 1);
                auto second = record_after_event(ready, values, 2);
                deck.start(first);
                deck.start(second);
                deck.run_until_idle();

                expect(values.empty());

                ready.set();
                deck.run_until_idle();

                expect(values == std::vector<int>{1, 2});
                expect(first.done());
                expect(second.done());
            };

            "reset makes future awaits suspend again"_test = [] {
                auto deck = nxt::rt::deck{};
                auto ready = nxt::rt::event{};
                auto values = std::vector<int>{};

                ready.set();
                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    co_await ready;
                    values.push_back(1);
                });

                ready.reset();

                auto task = record_after_event(ready, values, 2);

                deck.start(task);
                deck.run_until_idle();

                expect(values == std::vector<int>{1});
                expect(!task.done());

                ready.set();
                deck.run_until_idle();

                expect(values == std::vector<int>{1, 2});
                expect(task.done());
            };
        };

        "HTTP requests"_test = [] {
            "parse simple URLs"_test = [] {
                auto url = nxt::rt::http::parse_url(
                    "http://example.test:8080/path?q=1");

                expect(!url.tls);
                expect(url.host == "example.test");
                expect(url.port == "8080");
                expect(url.target == "/path?q=1");
                expect(nxt::rt::http::host_header(url)
                       == "example.test:8080");
            };

            "serialize HTTP/1.1 requests"_test = [] {
                auto wire = nxt::rt::http::serialize(
                    nxt::rt::http::request{
                        .method = "GET",
                        .target = "/hello",
                        .host = "example.test",
                        .headers = {{"Accept", "*/*"}},
                        .body = {},
                    });

                expect(wire == "GET /hello HTTP/1.1\r\n"
                               "Host: example.test\r\n"
                               "Accept: */*\r\n"
                               "Content-Length: 0\r\n"
                               "Connection: close\r\n"
                               "\r\n");
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

            "server-sent events parse through response body readers"_test = [] {
                auto deck = nxt::rt::deck{};
                auto chunks = std::array{
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "38\r\n"
                    "event: response.output_text.delta\n"
                    "data: {\"delta\":\"hi\"}\n"
                    "\n"
                    "\r\n"
                    "0\r\n\r\n"sv,
                };
                auto source = nxt::rt::string_source{std::span{chunks}};
                auto head_storage = std::array<std::byte, 256>{};
                auto reader =
                    nxt::rt::byte_reader{source, std::span{head_storage}};

                deck.sync_wait([&]() -> nxt::rt::task<void> {
                    auto head =
                        co_await nxt::rt::http::read_response_head(reader);
                    auto body =
                        nxt::rt::http::read_response_body(reader, head);
                    auto body_storage = std::array<std::byte, 256>{};
                    auto body_reader =
                        nxt::rt::byte_reader{body, std::span{body_storage}};

                    auto event =
                        co_await nxt::rt::http::parse_sse_event(body_reader);
                    expect(event.has_value());
                    expect(event->type == "response.output_text.delta");
                    expect(event->data == "{\"delta\":\"hi\"}");

                    auto end =
                        co_await nxt::rt::http::parse_sse_event(body_reader);
                    expect(!end);
                });
            };
        };
    }};

} // namespace nxt::test
