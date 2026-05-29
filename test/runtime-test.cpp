#include <nxt/sparkline.hpp>
#include <nxtui/tui_text.hpp>
#include <nxtrt/buffers.hpp>
#include <nxtrt/channel.hpp>
#include <nxtrt/event.hpp>
#include <nxtrt/http.hpp>
#include <nxtrt/kqueue_wand.hpp>
#include <nxtrt/sampling.hpp>
#include <nxtrt/task.hpp>
#include <nxtrt/terminal_app.hpp>
#include <nxtrt/ui_runtime.hpp>
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
#include <utility>
#include <vector>

namespace nxt::test {

using namespace nxtui;

using namespace boost::ut;
using namespace std::literals;

template<std::ranges::viewable_range Range>
auto text_source(Range && chunks, std::span<std::byte> storage)
{
    return nxtrt::byte_span_source{
        std::forward<Range>(chunks),
        storage,
    };
}

struct ambient_int_key
{
    using value_type = int;
    static constexpr auto name = "ambient-int";
};

struct manual_wand final : nxtrt::wand
{
    void
    suspend(nxtrt::wait_token token, nxtrt::parked_task task) override
    {
        parked.push_back(
            parked_entry{
                .token = token,
                .task = task,
            });
    }

    void cancel(nxtrt::wait_token token) override
    {
        cancelled.push_back(token);
    }

    void wave(nxtrt::deck &) override
    {
        ++waves;
    }

    void fulfill(nxtrt::deck & deck, nxtrt::wait_token token)
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
    nxtrt::wait_token prepare_wish(
        nxtrt::deck &,
        nxtrt::detail::promise_base &,
        nxtrt::detail::prepared_wish packet) override
    {
        auto * wish = std::get_if<nxtrt::op::manual>(&packet.wish);
        if (wish == nullptr)
            throw std::runtime_error{
                "manual_wand only implements manual wishes"};

        prepared.push_back(wish->token);
        states.push_back(
            std::static_pointer_cast<nxtrt::wait_state<void>>(
                packet.state));
        return wish->token;
    }

public:
    struct parked_entry
    {
        nxtrt::wait_token token = 0;
        nxtrt::parked_task task;
    };

    std::vector<nxtrt::wait_token> prepared;
    std::vector<nxtrt::wait_token> cancelled;
    std::vector<parked_entry> parked;
    std::vector<std::shared_ptr<nxtrt::wait_state<void>>> states;
    int waves = 0;
};

struct empty_then_string_source final : nxtrt::byte_reader
{
    explicit empty_then_string_source(std::size_t buffer_size = 8)
        : nxtrt::byte_reader(buffer_size)
    {}

private:
    nxtrt::hope<nxtrt::read_result> read_more() override
    {
        auto dst = unused_capacity();
        if (!returned_empty) {
            returned_empty = true;
            return nxtrt::hope<nxtrt::read_result>::ready(
                nxtrt::read_result{
                .bytes = 0,
                .eof = false,
            });
        }

        if (offset == text.size()) {
            return nxtrt::hope<nxtrt::read_result>::ready(
                nxtrt::read_result{
                .bytes = 0,
                .eof = true,
            });
        }

        auto rest = std::string_view{text}.substr(offset);
        auto n = std::min(dst.size(), rest.size());
        std::memcpy(dst.data(), rest.data(), n);
        offset += n;
        append_read_bytes(n);
        return nxtrt::hope<nxtrt::read_result>::ready(
            nxtrt::read_result{
            .bytes = n,
            .eof = offset == text.size(),
        });
    }

public:
    std::string_view text = "abc";
    std::size_t offset = 0;
    bool returned_empty = false;
};

struct chunking_string_sink final : nxtrt::byte_writer
{
    explicit chunking_string_sink(
        std::size_t limit,
        std::size_t buffer_size = 64)
        : nxtrt::byte_writer(buffer_size)
        , limit(limit)
    {}

    chunking_string_sink(
        std::size_t limit,
        std::span<std::byte> buffer)
        : nxtrt::byte_writer(buffer)
        , limit(limit)
    {}

private:
    nxtrt::hope<std::size_t>
    drain_more(
        std::span<const std::span<const std::byte>> chunks) override
    {
        auto remaining = limit;
        auto n = std::size_t{0};
        for (auto chunk : chunks) {
            auto take = std::min(remaining, chunk.size());
            text += nxtrt::as_string_view(chunk.first(take));
            n += take;
            remaining -= take;
            if (remaining == 0)
                break;
        }
        return nxtrt::hope<std::size_t>::ready(n);
    }

public:
    std::string text;
    std::size_t limit = 1;
};

struct shared_string_sink final : nxtrt::byte_writer
{
    explicit shared_string_sink(
        std::shared_ptr<std::string> text,
        std::size_t limit = 64,
        std::size_t buffer_size = 64)
        : nxtrt::byte_writer(buffer_size)
        , text(std::move(text))
        , limit(limit)
    {}

    shared_string_sink(
        std::shared_ptr<std::string> text,
        std::size_t limit,
        std::span<std::byte> buffer)
        : nxtrt::byte_writer(buffer)
        , text(std::move(text))
        , limit(limit)
    {}

private:
    nxtrt::hope<std::size_t>
    drain_more(
        std::span<const std::span<const std::byte>> chunks) override
    {
        auto remaining = limit;
        auto n = std::size_t{0};
        for (auto chunk : chunks) {
            auto take = std::min(remaining, chunk.size());
            *text += nxtrt::as_string_view(chunk.first(take));
            n += take;
            remaining -= take;
            if (remaining == 0)
                break;
        }
        return nxtrt::hope<std::size_t>::ready(n);
    }

public:
    std::shared_ptr<std::string> text;
    std::size_t limit = 1;
};

struct echo_tool
{
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Echo text.";
    static constexpr bool strict = true;

    struct parameters
    {
        std::string text;
    };

    static constexpr std::string_view parameters_schema_json =
        R"json({"type":"object","properties":{"text":{"type":"string","description":"Text to echo."}},"additionalProperties":false,"required":["text"]})json";

    static std::optional<parameters> parse_parameters(std::string_view json)
    {
        auto text = nxtai::tools::json_string_member(json, "text");
        if (!text)
            return std::nullopt;
        return parameters{.text = std::move(*text)};
    }

    nxtrt::task<nxtai::tools::tool_result> run(parameters args) const
    {
        co_await nxtrt::yield();
        co_return nxtai::tools::tool_result{
            .output = std::move(args.text),
            .observed = std::nullopt,
        };
    }
};

nxtrt::task<int> read_ambient_int_after_yield()
{
    co_await nxtrt::yield();
    co_return nxtrt::env_require<ambient_int_key>();
}

nxtrt::task<int> read_ambient_int()
{
    co_return nxtrt::env_require<ambient_int_key>();
}

nxtrt::task<void> record_after_yield(std::vector<int> & events, int value)
{
    events.push_back(value * 10 + 1);
    co_await nxtrt::yield();
    events.push_back(value * 10 + 2);
}

nxtrt::task<void>
record_next_channel_value(
    nxtrt::channel<int> & events,
    std::vector<int> & out)
{
    auto value = co_await events.next();
    if (value)
        out.push_back(*value);
}

nxtrt::task<void>
record_closed_channel(nxtrt::channel<int> & events, bool & finished)
{
    auto value = co_await events.next();
    expect(!value);
    finished = true;
}

nxtrt::task<void>
record_after_event(
    nxtrt::event & ready,
    std::vector<int> & out,
    int value)
{
    co_await ready;
    out.push_back(value);
}

nxtrt::task<void> record_current_zone(
    std::vector<nxtrt::task_zone *> & zones)
{
    co_await nxtrt::yield();
    zones.push_back(nxtrt::current_zone());
}

nxtrt::task<bool> read_task_stop_after_yield()
{
    co_await nxtrt::yield();
    co_return nxtrt::task_stop_requested();
}

nxtrt::task<bool> shielded_child_stop_state()
{
    co_return co_await nxtrt::shield(read_task_stop_after_yield());
}

nxtrt::task<void> await_manual_token(nxtrt::wait_token token)
{
    co_await nxtrt::op::manual{.token = token};
}

nxtrt::task<void> shielded_manual_token(nxtrt::wait_token token)
{
    co_await nxtrt::shield(await_manual_token(token));
}

nxtrt::task<void> throw_after_yield(std::vector<int> & events, int value)
{
    events.push_back(value * 10 + 1);
    co_await nxtrt::yield();
    throw nxtrt::runtime_error{"zone child boom"};
}

nxtrt::task<int> value_after_yield(int value)
{
    co_await nxtrt::yield();
    co_return value;
}

nxtrt::task<std::string> string_after_yield(std::string value)
{
    co_await nxtrt::yield();
    co_return value;
}

nxtrt::task<int> value_after_two_yields_or_stop(
    std::vector<int> & events,
    int value)
{
    co_await nxtrt::yield();
    co_await nxtrt::yield();
    if (nxtrt::stop_requested()) {
        events.push_back(value);
        throw nxtrt::operation_cancelled{};
    }
    co_return -value;
}

nxtrt::task<int> throw_int_after_yield()
{
    co_await nxtrt::yield();
    throw nxtrt::runtime_error{"zone child int boom"};
}

nxtrt::task<void> record_stop_state_after_yield(
    std::vector<int> & events,
    int value)
{
    co_await nxtrt::yield();
    events.push_back(nxtrt::stop_requested() ? value : -value);
}

nxtrt::task<void> record_stop_state_after_two_yields(
    std::vector<int> & events,
    int value)
{
    co_await nxtrt::yield();
    co_await nxtrt::yield();
    events.push_back(nxtrt::stop_requested() ? value : -value);
}

nxtrt::task<void> record_task_stop_state_after_yield(
    std::vector<int> & events,
    int value)
{
    co_await nxtrt::yield();
    events.push_back(nxtrt::task_stop_requested() ? value : -value);
}

nxtrt::task<void> draw_busy_and_mark(bool & ran)
{
    co_await nxtrt::draw(nxtui::tui::text("busy"));
    ran = true;
    co_return;
}

nxtrt::task<void> spawn_widget_and_capture(
    nxtrt::widget_slot & captured,
    bool & ran)
{
    auto child = nxtrt::spawn_widget(
        [&ran] {
            return draw_busy_and_mark(ran);
        });
    captured = child.surface();
    co_await nxtrt::draw(captured);
    co_return;
}

nxtrt::task<void> run_spawned_widget_clear_test(
    nxtrt::ui_runtime & runtime,
    nxtrt::widget_slot root,
    nxtrt::widget_slot & captured,
    bool & ran)
{
    auto body = [&] {
        return nxtrt::with_env<nxtrt::current_ui_runtime_key>(
            &runtime,
            [&] {
                return nxtrt::with_widget_slot(
                    root,
                    [&] {
                        return spawn_widget_and_capture(captured, ran);
                    });
            });
    };
    co_await nxtrt::with_zone(body);
}

nxtrt::task<int> draw_work_measure_and_stop(
    nxtrt::widget_slot root,
    nxtui::height_t & composed_height)
{
    co_await nxtrt::draw(nxtui::tui::text("work"));
    co_await nxtrt::yield();
    composed_height = root.height_hint().min;
    nxtrt::require_current_zone().stop();
    co_return 7;
}

nxtrt::task<void> draw_rate_until_stopped(bool & companion_stopped)
{
    co_await nxtrt::draw(nxtui::tui::text("rate"));
    while (!nxtrt::stop_requested())
        co_await nxtrt::yield();
    companion_stopped = true;
}

nxtrt::task<nxtrt::catching_deed<int>> spawn_sibling_widgets(
    nxtrt::widget_slot root,
    bool & companion_stopped,
    nxtui::height_t & composed_height)
{
    auto worker = nxtrt::spawn_widget(
        [root, &composed_height] {
            return draw_work_measure_and_stop(root, composed_height);
        });
    auto companion = nxtrt::spawn_widget(
        [&companion_stopped] {
            return draw_rate_until_stopped(companion_stopped);
        });
    co_await nxtrt::draw(
        nxtui::tui::column(worker.surface(), companion.surface()));
    co_return std::move(worker).cope();
}

nxtrt::task<nxtrt::catching_deed<int>>
run_sibling_widget_compose_test(
    nxtrt::ui_runtime & runtime,
    nxtrt::widget_slot root,
    bool & companion_stopped,
    nxtui::height_t & composed_height)
{
    auto body = [&] {
        return nxtrt::with_env<nxtrt::current_ui_runtime_key>(
            &runtime,
            [&] {
                return nxtrt::with_widget_slot(
                    root,
                    [&] {
                        return spawn_sibling_widgets(
                            root,
                            companion_stopped,
                            composed_height);
                    });
            });
    };
    co_return co_await nxtrt::with_zone(body);
}

nxtrt::task<bool> draw_after_stopping_current_zone()
{
    nxtrt::require_current_zone().stop();
    try {
        co_await nxtrt::draw(nxtui::tui::text("after-stop"));
    } catch (const nxtrt::operation_cancelled &) {
        co_return true;
    }
    co_return false;
}

nxtrt::task<bool> print_after_stopping_current_zone()
{
    nxtrt::require_current_zone().stop();
    try {
        co_await nxtrt::print_block("after-stop\n");
    } catch (const nxtrt::operation_cancelled &) {
        co_return true;
    }
    co_return false;
}

nxtrt::task<bool> run_draw_after_stop_check(
    nxtrt::ui_runtime & runtime,
    nxtrt::widget_slot root)
{
    auto body = [&] {
        return nxtrt::with_env<nxtrt::current_ui_runtime_key>(
            &runtime,
            [&] {
                return nxtrt::with_widget_slot(
                    root,
                    [] {
                        return draw_after_stopping_current_zone();
                    });
            });
    };
    co_return co_await nxtrt::with_zone(body);
}

nxtrt::task<bool> run_print_after_stop_check(
    nxtrt::ui_runtime & runtime,
    nxtrt::widget_slot root)
{
    auto body = [&] {
        return nxtrt::with_env<nxtrt::current_ui_runtime_key>(
            &runtime,
            [&] {
                return nxtrt::with_widget_slot(
                    root,
                    [] {
                        return print_after_stopping_current_zone();
                    });
            });
    };
    co_return co_await nxtrt::with_zone(body);
}

// A custom awaitable exercising task::splice_onto. When `ready` holds it
// resolves through await_ready() and never suspends; on a miss it delegates
// its slow path to a real task spliced as the awaiter's continuation. This is
// the buffered-reader fast/slow split in miniature: a buffered read takes the
// ready path with no deck round-trip, a miss runs a refill coroutine.
inline nxtrt::task<void> splice_probe_fill(std::vector<int> & events)
{
    events.push_back(10);
    co_await nxtrt::yield();
    events.push_back(11);
}

struct splice_probe
{
    bool ready = false;
    int value = 0;
    std::vector<int> & events;
    int & suspends;
    nxtrt::task<void> slow_{};

    [[nodiscard]] bool await_ready() const noexcept
    {
        return ready;
    }

    void await_suspend(std::coroutine_handle<> awaiting)
    {
        ++suspends;
        slow_ = splice_probe_fill(events);
        slow_.splice_onto(awaiting);
    }

    int await_resume()
    {
        if (slow_.handle())
            slow_.handle().promise().result();
        return value;
    }
};

inline nxtrt::task<int>
run_splice_probe(std::vector<int> & events, int & suspends, bool ready)
{
    auto value = co_await splice_probe{
        .ready = ready,
        .value = 42,
        .events = events,
        .suspends = suspends,
    };
    events.push_back(99);
    co_return value;
}

// The fast/slow split with `hope`: a ready hope returns its value inline,
// while the pending arm is a task that loops before producing the value.
inline nxtrt::task<int> hope_refill(std::vector<int> & events)
{
    events.push_back(10);
    co_await nxtrt::yield();
    events.push_back(11);
    co_return 42;
}

inline nxtrt::task<int>
run_hope(std::vector<int> & events, int & suspends, bool ready)
{
    auto pick = [&]() -> nxtrt::hope<int> {
        if (ready)
            return nxtrt::hope<int>::ready(42);
        ++suspends;
        return nxtrt::hope<int>{hope_refill(events)};
    };
    auto value = co_await pick();
    events.push_back(99);
    co_return value;
}

inline nxtrt::task<std::string>
take_three_buffered_bytes(nxtrt::byte_reader & reader, std::vector<int> & events)
{
    co_await reader.fill(3);
    events.push_back(1);
    auto first = co_await reader.take(1);
    events.push_back(2);
    auto second = co_await reader.take(1);
    events.push_back(3);
    auto third = co_await reader.take(1);
    events.push_back(4);

    auto out = std::string{};
    out += nxtrt::as_string_view(first);
    out += nxtrt::as_string_view(second);
    out += nxtrt::as_string_view(third);
    co_return out;
}

inline nxtrt::task<void>
write_three_buffered_bytes(nxtrt::byte_writer & writer, std::vector<int> & events)
{
    events.push_back(1);
    co_await writer.write("a"sv);
    events.push_back(2);
    co_await writer.write("b"sv);
    events.push_back(3);
    co_await writer.write("c"sv);
    events.push_back(4);
}

inline nxtrt::task<void> flush_writer(nxtrt::byte_writer & writer)
{
    co_await writer.flush();
}

// `map` over the three awaitable shapes: a plain task, a synchronously-ready
// hope, and a real wand wish.
inline nxtrt::task<int> map_over_task()
{
    co_return co_await (
        value_after_yield(21) | nxtrt::map([](int x) { return x + 1; }));
}

inline nxtrt::task<int> map_over_ready_hope()
{
    co_return co_await nxtrt::map(
        nxtrt::hope<int>::ready(21), [](int x) { return x * 2; });
}

inline nxtrt::task<int> map_over_manual_wish(nxtrt::wait_token token)
{
    co_return co_await nxtrt::map(
        nxtrt::op::manual{.token = token}, [] { return 7; });
}

static suite runtime_tests{
    "Runtime", [] {
        "charting"_test = [] {
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

        "text flow"_test = [] {
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

        "deck"_test = [] {
            "sync_wait returns completed root task values"_test = [] {
                auto deck = nxtrt::deck{};

                expect(deck.sync_wait([]() -> nxtrt::task<int> {
                    co_return 7;
                }) == 7_i);
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
                auto deck = nxtrt::deck{};
                auto out = std::vector<int>{};

                auto child_body = [&out](int tag) -> nxtrt::task<void> {
                    out.push_back(tag * 10 + 1);
                    co_await nxtrt::yield();
                    out.push_back(tag * 10 + 2);
                };

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    auto first = child_body(1);
                    auto second = child_body(2);

                    co_await first;
                    co_await second;
                });

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

                auto task = task_body(events);
                deck.start(task);
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

                auto task = []() -> nxtrt::task<bool> {
                    co_return nxtrt::task_stop_requested();
                }();
                task.request_stop();

                expect(deck.sync_wait(std::move(task)));
            };

            "ready awaitable resolves without suspending"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto suspends = 0;

                auto task = run_splice_probe(events, suspends, true);
                deck.start(task);
                deck.run_ready();

                expect(suspends == 0_i)
                    << "ready fast path must not enter await_suspend";
                expect(task.done())
                    << "ready awaitable should finish in one pump round";
                expect(deck.empty())
                    << "ready awaitable should not queue any work";
                expect(events == std::vector<int>{99})
                    << "ready path should skip the delegate task entirely";
                expect(std::move(task).result() == 42_i);
            };

            "missing awaitable delegates to a spliced task"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto suspends = 0;

                auto value = deck.sync_wait(
                    run_splice_probe(events, suspends, false));

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

                auto task = run_hope(events, suspends, true);
                deck.start(task);
                deck.run_ready();

                expect(suspends == 0_i)
                    << "ready hope must not build a delegate task";
                expect(task.done())
                    << "ready hope should finish in one pump round";
                expect(deck.empty());
                expect(events == std::vector<int>{99});
                expect(std::move(task).result() == 42_i);
            };

            "hope makes a wish and resumes after it"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto suspends = 0;

                auto value =
                    deck.sync_wait(run_hope(events, suspends, false));

                expect(suspends == 1_i)
                    << "miss path should build exactly one delegate task";
                expect(value == 42_i);
                expect(events == std::vector<int>{10, 11, 99})
                    << "delegate wish must finish before the value is read";
            };

            "map transforms a task result"_test = [] {
                auto deck = nxtrt::deck{};
                expect(deck.sync_wait(map_over_task()) == 22_i);
            };

            "map over a ready hope stays synchronous"_test = [] {
                auto deck = nxtrt::deck{};

                auto task = map_over_ready_hope();
                deck.start(task);
                deck.run_ready();

                expect(task.done())
                    << "mapping a ready awaitable must not add a suspension";
                expect(deck.empty());
                expect(std::move(task).result() == 42_i);
            };

            "map transforms a wish result over one round-trip"_test = [] {
                auto wand = manual_wand{};
                auto deck = nxtrt::deck{&wand};

                auto task = map_over_manual_wish(55);
                deck.start(task);
                deck.run_ready();

                expect(!task.done())
                    << "mapped wish should park, not complete synchronously";
                expect(wand.prepared == std::vector<nxtrt::wait_token>{55})
                    << "map must forward the wish's preparation to the wand";
                expect(wand.parked.size() == std::size_t{1})
                    << "map must forward the single suspension";

                wand.fulfill(deck, 55);
                deck.run_ready();

                expect(task.done());
                expect(std::move(task).result() == 7_i)
                    << "transform must run in the resume after fulfillment";
            };

            "then transforms task values"_test = [] {
                auto deck = nxtrt::deck{};

                auto result = deck.sync_wait(
                    nxtrt::then(value_after_yield(20), [](int value) {
                        return value + 1;
                    }));

                expect(result == 21_i);
            };

            "let_value chains task values"_test = [] {
                auto deck = nxtrt::deck{};

                auto result = deck.sync_wait(
                    nxtrt::let_value(value_after_yield(20), [](int value) {
                        return value_after_yield(value + 2);
                    }));

                expect(result == 22_i);
            };

            "finally runs shielded cleanup before returning values"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                auto result = deck.sync_wait(nxtrt::finally(
                    value_after_yield(7),
                    [&]() {
                        return record_after_yield(events, 9);
                    }));

                expect(result == 7_i);
                expect(events == std::vector<int>{91, 92});
            };

            "finally runs cleanup after body failure"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    (void)deck.sync_wait(nxtrt::finally(
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
                auto deck = nxtrt::deck{};
                auto grouped = false;

                try {
                    (void)deck.sync_wait(nxtrt::finally(
                        throw_int_after_yield(),
                        []() -> nxtrt::task<void> {
                            co_await nxtrt::yield();
                            throw nxtrt::runtime_error{"cleanup boom"};
                        }));
                } catch (const nxtrt::exception_group & group) {
                    grouped = true;
                    expect(group.exceptions().size() == std::size_t{2});
                }

                expect(grouped);
            };

            "task adaptors flow through then and let_value"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                auto result = deck.sync_wait(
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

            "for_each_task awaits lazy ranges of tasks"_test = [] {
                auto deck = nxtrt::deck{};
                auto values = std::array{1, 2, 3};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::for_each_task(
                        values | std::views::transform(
                            [&](int value) {
                                return record_after_yield(events, value);
                            }));
                });

                expect(events == std::vector<int>{11, 12, 21, 22, 31, 32});
            };

            "when_all_range awaits lazy ranges concurrently"_test = [] {
                auto deck = nxtrt::deck{};
                auto values = std::array{1, 2, 3};

                auto result = deck.sync_wait([&]() -> nxtrt::task<std::vector<int>> {
                    co_return co_await nxtrt::when_all_range(
                        values | std::views::transform(
                            [](int value) {
                                return value_after_yield(value * 10);
                            }));
                });

                expect(result == std::vector<int>{10, 20, 30});
            };

            "wait_any_range awaits lazy ranges concurrently"_test = [] {
                auto deck = nxtrt::deck{};
                auto values = std::array{5, 6};
                auto events = std::vector<int>{};

                auto result = deck.sync_wait([&]() -> nxtrt::task<int> {
                    co_return co_await nxtrt::wait_any_range(
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
            auto deck = nxtrt::deck{};

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

            "survives nested task awaits"_test = [&] {
                auto result = deck.sync_wait([]() -> nxtrt::task<int> {
                    co_return co_await nxtrt::with_env<ambient_int_key>(
                        41, [] { return read_ambient_int_after_yield(); });
                });

                expect(result == 41_i);
            };

            "restores outer env values"_test = [&] {
                auto result = deck.sync_wait([]() -> nxtrt::task<int> {
                    co_return co_await nxtrt::with_env<ambient_int_key>(
                        10, []() -> nxtrt::task<int> {
                            auto before = co_await read_ambient_int();
                            auto inside = co_await nxtrt::with_env<
                                ambient_int_key>(20, [] {
                                return read_ambient_int_after_yield();
                            });
                            auto after = co_await read_ambient_int();
                            co_return before * 100 + inside * 10 + after;
                        });
                });

                expect(result == 1210_i);
            };

            "forked tasks keep env after binder exits"_test = [&] {
                auto child =
                    deck.sync_wait([]()
                        -> nxtrt::task<nxtrt::catching_deed<int>> {
                        co_return co_await nxtrt::with_zone(
                            []()
                                -> nxtrt::task<
                                    nxtrt::catching_deed<int>> {
                                co_return co_await nxtrt::with_env<
                                    ambient_int_key>(
                                    99, []()
                                        -> nxtrt::task<
                                            nxtrt::catching_deed<int>> {
                                        auto child =
                                            nxtrt::fork(
                                                read_ambient_int_after_yield())
                                                .cope();
                                        co_return std::move(child);
                                    });
                            });
                    });

                auto result = std::move(child).get();
                expect(result.has_value());
                expect(*result == 99_i);
            };

            "trace context is inherited by forked tasks"_test = [&] {
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

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_env<nxtrt::trace_context_key>(
                        trace, [&]() -> nxtrt::task<void> {
                        co_await nxtrt::with_env<
                            nxtrt::trace_current_span_key>(
                            root.span_id(), [&]() -> nxtrt::task<void> {
                            co_await nxtrt::with_zone(
                                [&]() -> nxtrt::task<void> {
                                nxtrt::fork(traced_child("child-a"));
                                nxtrt::fork(traced_child("child-b"));
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
                auto trace = std::make_shared<nxtrt::trace_context>();
                auto root = trace->start_span("root");

                auto result = deck.sync_wait([&]() -> nxtrt::task<int> {
                    co_return co_await nxtrt::with_env<
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
                auto options = nxtrt::terminal_app_options{};
                expect(!options.alternate_screen);
            };
        };

        "widget slots"_test = [] {
            "child slots expose independently drawable surfaces"_test = [] {
                auto runtime = nxtrt::ui_runtime{
                    {.render = false,
                     .fallback_size = {16 * nxtui::ch, 4 * nxtui::ln}}};
                auto root = runtime.surface();
                auto child = runtime.make_surface();

                root.publish(
                    nxtui::tui::AnyLayout{
                        nxtui::tui::row(child, nxtui::tui::text("!"))});
                child.publish(nxtui::tui::AnyLayout{nxtui::tui::text("hi")});

                expect(child.width_hint().min == 2 * nxtui::ch);
                expect(root.width_hint().min == 3 * nxtui::ch);
            };

            "spawned child widgets clear their slot on exit"_test = [] {
                auto runtime = nxtrt::ui_runtime{
                    {.render = false,
                     .fallback_size = {16 * nxtui::ch, 4 * nxtui::ln}}};
                auto root = runtime.surface();
                auto captured =
                    nxtui::tui::Slot<nxtui::tui::AnyLayout>{nxtui::tui::AnyLayout{}};
                auto ran = false;
                auto deck = nxtrt::deck{};

                deck.sync_wait(
                    run_spawned_widget_clear_test(
                        runtime,
                        root,
                        captured,
                        ran));

                expect(ran);
                expect(captured.width_hint().min == 0 * nxtui::ch);
            };

            "ambient child widgets compose as siblings"_test = [] {
                auto runtime = nxtrt::ui_runtime{
                    {.render = false,
                     .fallback_size = {16 * nxtui::ch, 4 * nxtui::ln}}};
                auto root = runtime.surface();
                auto deck = nxtrt::deck{};
                auto companion_stopped = false;
                auto composed_height = 0 * nxtui::ln;

                auto worker_deed = deck.sync_wait(
                    run_sibling_widget_compose_test(
                        runtime,
                        root,
                        companion_stopped,
                        composed_height));

                auto result = std::move(worker_deed).get();
                expect(result.has_value());
                expect(*result == 7_i);
                expect(companion_stopped);
                expect(composed_height == 2 * nxtui::ln);
            };

            "draw observes zone stop before publishing"_test = [] {
                auto runtime = nxtrt::ui_runtime{
                    {.render = false,
                     .fallback_size = {16 * nxtui::ch, 4 * nxtui::ln}}};
                auto root = runtime.surface();
                auto deck = nxtrt::deck{};

                auto cancelled = deck.sync_wait(
                    run_draw_after_stop_check(runtime, root));

                expect(cancelled);
                expect(root.width_hint().min == 0 * nxtui::ch);
            };

            "print observes zone stop before enqueueing"_test = [] {
                auto runtime = nxtrt::ui_runtime{
                    {.render = false,
                     .fallback_size = {16 * nxtui::ch, 4 * nxtui::ln}}};
                auto root = runtime.surface();
                auto deck = nxtrt::deck{};

                auto cancelled = deck.sync_wait(
                    run_print_after_stop_check(runtime, root));

                expect(cancelled);
            };
        };

        "zones"_test = [] {
            "bind the current zone while the body runs"_test = [] {
                auto deck = nxtrt::deck{};

                auto seen = deck.sync_wait([]() -> nxtrt::task<bool> {
                    co_return co_await nxtrt::with_zone(
                        []() -> nxtrt::task<bool> {
                            auto * before = nxtrt::current_zone();
                            co_await nxtrt::yield();
                            auto * after = nxtrt::current_zone();
                            co_return before != nullptr && before == after;
                        });
                });

                expect(seen);
            };

            "join forked tasks before the zone exits"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        nxtrt::fork(record_after_yield(events, 1));
                        events.push_back(2);
                        co_return;
                    });
                    events.push_back(3);
                    co_return;
                });

                expect(events == std::vector<int>{2, 11, 12, 3});
            };

            "let forked tasks inherit the current zone"_test = [] {
                auto deck = nxtrt::deck{};
                auto zones = std::vector<nxtrt::task_zone *>{};
                auto expected = static_cast<nxtrt::task_zone *>(nullptr);

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        expected = nxtrt::current_zone();
                        nxtrt::fork(record_current_zone(zones));
                        co_return;
                    });
                    co_return;
                });

                expect(expected != nullptr);
                expect(zones == std::vector<nxtrt::task_zone *>{expected});
            };

            "allow children to fork more work into the same zone"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                auto parent = [&events]() -> nxtrt::task<void> {
                    events.push_back(1);
                    co_await nxtrt::yield();
                    nxtrt::fork(record_after_yield(events, 2));
                    events.push_back(3);
                    co_return;
                };

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        nxtrt::fork(parent());
                        co_return;
                    });
                    events.push_back(4);
                    co_return;
                });

                expect(events == std::vector<int>{1, 3, 21, 22, 4});
            };

            "reject fork outside a zone"_test = [] {
                auto deck = nxtrt::deck{};

                auto rejected = deck.sync_wait([]() -> nxtrt::task<bool> {
                    try {
                        nxtrt::fork([]() -> nxtrt::task<void> {
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
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    deck.sync_wait([&]() -> nxtrt::task<void> {
                        co_await nxtrt::with_zone(
                            [&]() -> nxtrt::task<void> {
                                nxtrt::fork(throw_after_yield(events, 0));
                                nxtrt::fork(record_after_yield(events, 2));
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
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto grouped = false;

                try {
                    deck.sync_wait([&]() -> nxtrt::task<void> {
                        co_await nxtrt::with_zone(
                            [&]() -> nxtrt::task<void> {
                                nxtrt::fork(throw_after_yield(events, 1));
                                nxtrt::fork(throw_after_yield(events, 2));
                                nxtrt::fork(record_after_yield(events, 3));
                                co_return;
                            });
                        co_return;
                    });
                } catch (const nxtrt::exception_group & group) {
                    grouped = true;
                    expect(group.exceptions().size() == std::size_t{2});
                }

                expect(grouped);
                expect(events == std::vector<int>{11, 21, 31, 32});
            };

            "return forked task results after joining"_test = [] {
                auto deck = nxtrt::deck{};

                auto child =
                    deck.sync_wait([]()
                        -> nxtrt::task<nxtrt::deed<int>> {
                        co_return co_await nxtrt::with_zone(
                            []() -> nxtrt::task<nxtrt::deed<int>> {
                                auto child =
                                    nxtrt::fork(value_after_yield(42));
                                co_return std::move(child);
                            });
                    });

                expect(std::move(child).get() == 42_i);
            };

            "return several forked task results"_test = [] {
                auto deck = nxtrt::deck{};
                using children_type = std::tuple<
                    nxtrt::deed<int>,
                    nxtrt::deed<int>>;

                auto children =
                    deck.sync_wait([]() -> nxtrt::task<children_type> {
                        co_return co_await nxtrt::with_zone(
                            []() -> nxtrt::task<children_type> {
                                auto first =
                                    nxtrt::fork(value_after_yield(10));
                                auto second =
                                    nxtrt::fork(value_after_yield(20));
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
                auto deck = nxtrt::deck{};

                auto child =
                    deck.sync_wait([]()
                        -> nxtrt::task<nxtrt::deed<int>> {
                        co_return co_await nxtrt::with_zone(
                            []() -> nxtrt::task<nxtrt::deed<int>> {
                                auto child =
                                    nxtrt::fork(throw_int_after_yield());
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
                auto deck = nxtrt::deck{};
                auto observed = false;

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        auto child =
                            nxtrt::fork(throw_int_after_yield());
                        co_await nxtrt::yield();
                        co_await nxtrt::yield();
                        observed = child.exception() != nullptr;
                        co_return;
                    });
                });

                expect(observed);
            };

            "let coped deeds report failure as expected"_test = [] {
                auto deck = nxtrt::deck{};

                auto child =
                    deck.sync_wait([]()
                        -> nxtrt::task<nxtrt::catching_deed<int>> {
                        co_return co_await nxtrt::with_zone(
                            []()
                                -> nxtrt::task<
                                    nxtrt::catching_deed<int>> {
                                auto child =
                                    nxtrt::fork(throw_int_after_yield())
                                        .cope();
                                co_return std::move(child);
                            });
                    });

                auto result = std::move(child).get();
                expect(!result.has_value());
            };

            "let coped deeds report success as expected"_test = [] {
                auto deck = nxtrt::deck{};

                auto child =
                    deck.sync_wait([]()
                        -> nxtrt::task<nxtrt::catching_deed<int>> {
                        co_return co_await nxtrt::with_zone(
                            []()
                                -> nxtrt::task<
                                    nxtrt::catching_deed<int>> {
                                auto child =
                                    nxtrt::fork(value_after_yield(99))
                                        .cope();
                                co_return std::move(child);
                            });
                    });

                auto result = std::move(child).get();
                expect(result.has_value());
                expect(*result == 99_i);
            };

            "share a stop token with forked children"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        nxtrt::fork(
                            record_stop_state_after_yield(events, 1));
                        nxtrt::require_current_zone().stop();
                        co_return;
                    });
                });

                expect(events == std::vector<int>{1});
            };

            "reject fork after zone stop"_test = [] {
                auto deck = nxtrt::deck{};
                auto rejected = false;

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        nxtrt::require_current_zone().stop();
                        try {
                            nxtrt::fork(value_after_yield(1));
                        } catch (const std::exception &) {
                            rejected = true;
                        }
                        co_return;
                    });
                });

                expect(rejected);
            };

            "request child stop when the zone body fails"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    deck.sync_wait([&]() -> nxtrt::task<void> {
                        co_await nxtrt::with_zone(
                            [&]() -> nxtrt::task<void> {
                                nxtrt::fork(
                                    record_stop_state_after_yield(events, 2));
                                throw nxtrt::runtime_error{
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
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        nxtrt::fork(
                            record_task_stop_state_after_yield(events, 3));
                        nxtrt::require_current_zone().stop();
                        co_return;
                    });
                });

                expect(events == std::vector<int>{3});
            };

            "child cancellation is not a zone failure after stop"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        nxtrt::fork(
                            value_after_two_yields_or_stop(events, 4));
                        co_await nxtrt::yield();
                        nxtrt::require_current_zone().stop();
                    });
                });

                expect(events == std::vector<int>{4});
            };

            "child cancellation remains failure before zone stop"_test = [] {
                auto deck = nxtrt::deck{};
                auto threw = false;

                try {
                    deck.sync_wait([&]() -> nxtrt::task<void> {
                        co_await nxtrt::with_zone(
                            []() -> nxtrt::task<void> {
                                nxtrt::fork(
                                    []() -> nxtrt::task<void> {
                                        throw nxtrt::operation_cancelled{};
                                    }());
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
                auto task = shielded_child_stop_state();

                deck.start(task);
                task.request_stop();
                deck.run_until_idle();

                expect(task.done());
                expect(!std::move(task).result());
            };

            "shielded child wishes are not cancelled by parent stop"_test = [] {
                auto wand = manual_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = shielded_manual_token(99);

                deck.start(task);
                task.request_stop();
                deck.run_until_idle();

                expect(wand.prepared == std::vector<nxtrt::wait_token>{99});
                expect(wand.cancelled.empty());
                expect(wand.parked.size() == std::size_t{1});

                wand.fulfill(deck, 99);
                deck.run_until_idle();

                expect(task.done());
                std::move(task).result();
            };

            "stop a hosted zone when its parent task is stopped"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                auto task = [&]() -> nxtrt::task<void> {
                    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
                        nxtrt::fork(
                            record_stop_state_after_yield(events, 4));
                        events.push_back(100);
                        co_await nxtrt::yield();
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
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};
                auto threw = false;

                try {
                    deck.sync_wait([&]() -> nxtrt::task<void> {
                        co_await nxtrt::with_zone(
                            nxtrt::stop_on_failure{},
                            [&](auto & policy) -> nxtrt::task<void> {
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
                auto deck = nxtrt::deck{};
                auto events = std::vector<int>{};

                auto child =
                    deck.sync_wait([&]()
                        -> nxtrt::task<nxtrt::deed<int>> {
                        co_return co_await nxtrt::with_zone(
                            nxtrt::stop_on_success{},
                            [&](auto & policy)
                                -> nxtrt::task<nxtrt::deed<int>> {
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
        };

        "tool batches"_test = [] {
            "parse calls and return function_call_output items in order"_test = [] {
                auto deck = nxtrt::deck{};
                auto calls = deck.sync_wait(
                    nxtai::tools::read_function_calls_from_items(
                        {
                            nxtai::openai::raw_json{
                                R"({"id":"fc_1","type":"function_call","call_id":"call_1","name":"echo","arguments":"{\"text\":\"one\"}"})"},
                            nxtai::openai::raw_json{
                                R"({"id":"fc_2","type":"function_call","call_id":"call_2","name":"echo","arguments":"{\"text\":\"two\"}"})"},
                        }));
                auto tools = nxtai::tools::make_tool_registry({
                    nxtai::tools::make_function_tool(echo_tool{}),
                });

                auto results = deck.sync_wait(
                    nxtai::tools::run_function_tool_batch(
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
                auto deck = nxtrt::deck{};
                auto tools = nxtai::tools::make_tool_registry({
                    nxtai::tools::make_function_tool(echo_tool{}),
                });
                auto calls = std::vector<nxtai::tools::function_call>{
                    nxtai::tools::function_call{
                        .call_id = "call_missing",
                        .name = "missing",
                        .arguments = "{}",
                    },
                };

                auto results = deck.sync_wait(
                    nxtai::tools::run_function_tool_batch(
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
                auto deck = nxtrt::deck{&wand};
                auto events = std::vector<int>{};

                auto task_body =
                    [](std::vector<int> & events) -> nxtrt::task<void> {
                    events.push_back(1);
                    co_await nxtrt::op::manual{.token = 42};
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
                    wand.prepared == std::vector<nxtrt::wait_token>{42})
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
                auto deck = nxtrt::deck{&wand};
                auto events = std::vector<int>{};

                auto task_body =
                    [](std::vector<int> & events) -> nxtrt::task<void> {
                    events.push_back(1);
                    co_await nxtrt::op::manual{.token = 7};
                    events.push_back(2);
                };

                auto task = task_body(events);
                deck.start(task);
                deck.run_ready();

                expect(events == std::vector<int>{1});
                expect(
                    wand.prepared == std::vector<nxtrt::wait_token>{7});
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
                auto deck = nxtrt::deck{&wand};

                auto task = []() -> nxtrt::task<void> {
                    co_await nxtrt::op::manual{.token = 99};
                }();

                deck.start(task);
                deck.run_ready();
                task.request_stop();

                expect(wand.cancelled == std::vector<nxtrt::wait_token>{99});
            };
        };

        "buffers"_test = [] {
            "ema rate smooths byte deltas over time"_test = [] {
                auto rate = nxtrt::ema_rate{std::chrono::seconds{1}};

                auto first =
                    rate.sample(std::size_t{1000}, std::chrono::seconds{1});
                auto second =
                    rate.sample(std::size_t{0}, std::chrono::seconds{1});

                expect(first > 999.0);
                expect(first < 1001.0);
                expect(second > 499.0);
                expect(second < 501.0);
            };

            "chunks are visited through reused storage"_test = [] {
                auto deck = nxtrt::deck{};
                auto chunks = std::array{"ab"sv, "cdef"sv, "g"sv};
                auto storage = std::array<std::byte, 3>{};
                auto source = text_source(chunks, std::span{storage});
                auto visited = std::vector<std::string>{};

                auto total =
                    deck.sync_wait([&]() -> nxtrt::task<std::size_t> {
                        co_return co_await nxtrt::for_each_chunk(
                            source,
                            [&visited](std::span<const std::byte> chunk) {
                                visited.emplace_back(
                                    nxtrt::as_string_view(chunk));
                            });
                    });

                expect(total == std::size_t{7});
                expect(
                    visited == std::vector<std::string>{"abc", "def", "g"});
            };

            "byte span source accepts lazy ranges"_test = [] {
                auto deck = nxtrt::deck{};
                auto texts = std::array{"ab"sv, ""sv, "cde"sv, "f"sv};
                auto storage = std::array<std::byte, 3>{};
                auto source = nxtrt::byte_span_source{
                    texts
                        | std::views::filter([](std::string_view) {
                            return true;
                        }),
                    std::span{storage},
                };
                auto visited = std::vector<std::string>{};

                auto total =
                    deck.sync_wait([&]() -> nxtrt::task<std::size_t> {
                        co_return co_await nxtrt::for_each_chunk(
                            source,
                            [&visited](std::span<const std::byte> chunk) {
                                visited.emplace_back(
                                    nxtrt::as_string_view(chunk));
                            });
                    });

                expect(total == std::size_t{6});
                expect(visited == std::vector<std::string>{"abc", "def"});
            };

            "protocol leftovers remain buffered"_test = [] {
                auto deck = nxtrt::deck{};
                auto chunks = std::array{"abc--def--ghi"sv};
                auto storage = std::array<std::byte, 16>{};
                auto reader = text_source(chunks, std::span{storage});

                auto parts = deck.sync_wait(
                    [&]() -> nxtrt::task<std::vector<std::string>> {
                        auto out = std::vector<std::string>{};
                        out.emplace_back(
                            nxtrt::as_string_view(
                                co_await reader.take_until("--")));
                        out.emplace_back(
                            nxtrt::as_string_view(
                                co_await reader.take_until("--")));
                        out.emplace_back(
                            nxtrt::as_string_view(reader.buffered()));
                        co_return out;
                    });

                expect(
                    parts == std::vector<std::string>{"abc", "def", "ghi"});
            };

            "buffer hits do not add suspension"_test = [] {
                auto deck = nxtrt::deck{};
                auto chunks = std::array{"abc"sv};
                auto storage = std::array<std::byte, 3>{};
                auto reader = text_source(chunks, std::span{storage});
                auto events = std::vector<int>{};

                auto task = take_three_buffered_bytes(reader, events);
                deck.start(task);

                deck.run_ready();
                expect(!task.done())
                    << "initial fill should delegate to a slow task";
                deck.run_ready();
                expect(!task.done())
                    << "fill continuation should wait for the next deck turn";
                deck.run_ready();

                expect(task.done())
                    << "three buffered take(1) calls must finish inline";
                expect(events == std::vector<int>{1, 2, 3, 4});
                expect(std::move(task).result() == "abc");
            };

            "empty reads are distinguished from EOF"_test = [] {
                auto deck = nxtrt::deck{};
                auto storage = std::array<std::byte, 8>{};
                auto reader = empty_then_string_source{storage.size()};

                auto parts = deck.sync_wait(
                    [&]() -> nxtrt::task<std::vector<std::string>> {
                        auto out = std::vector<std::string>{};
                        while (auto chunk = co_await reader.take_some())
                            out.emplace_back(
                                nxtrt::as_string_view(*chunk));
                        co_return out;
                    });

                expect(parts == std::vector<std::string>{"", "abc"});
            };

            "byte_reader streams one chunk into a writer"_test = [] {
                auto deck = nxtrt::deck{};
                auto chunks = std::array{"abcdef"sv};
                auto source_storage = std::array<std::byte, 4>{};
                auto reader = text_source(chunks, std::span{source_storage});
                auto writer = chunking_string_sink{64, std::size_t{0}};

                auto streamed =
                    deck.sync_wait([&]() -> nxtrt::task<std::size_t> {
                    co_return (co_await reader.stream(writer, 3)).bytes;
                });

                expect(streamed == std::size_t{3});
                expect(writer.text == "abc");
                expect(reader.buffered_size() == std::size_t{1});
            };

            "byte_reader discards without exposing bytes"_test = [] {
                auto deck = nxtrt::deck{};
                auto chunks = std::array{"abcd"sv};
                auto storage = std::array<std::byte, 4>{};
                auto reader = text_source(chunks, std::span{storage});

                auto discarded =
                    deck.sync_wait([&]() -> nxtrt::task<std::size_t> {
                    co_return (co_await reader.discard(2)).bytes;
                });
                auto rest = deck.sync_wait([&]() -> nxtrt::task<std::string> {
                    co_return std::string{
                        nxtrt::as_string_view(co_await reader.take(2))};
                });
                auto eof = deck.sync_wait([&]() -> nxtrt::task<nxtrt::read_result> {
                    co_return co_await reader.discard();
                });

                expect(discarded == std::size_t{2});
                expect(rest == "cd");
                expect(eof.bytes == std::size_t{0});
                expect(eof.eof);
            };

            "write_all drains borrowed bytes into sinks"_test = [] {
                auto deck = nxtrt::deck{};
                auto sink = chunking_string_sink{2};

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    co_await nxtrt::write_all(
                        sink,
                        std::string{"abcdef"});
                });

                expect(sink.text == "abcdef");
            };

            "task_byte_source reads through a task callable"_test = [] {
                "from read results"_test = [] {
                    auto deck = nxtrt::deck{};
                    auto read = [](std::span<std::byte> dst)
                        -> nxtrt::task<nxtrt::read_result> {
                        auto text = std::string_view{"xy"};
                        std::memcpy(dst.data(), text.data(), text.size());
                        co_return nxtrt::read_result{
                            .bytes = text.size(),
                            .eof = true,
                        };
                    };
                    auto storage = std::array<std::byte, 4>{};
                    auto source = nxtrt::task_byte_source{read, std::span{storage}};

                    auto result = deck.sync_wait(
                        [&]() -> nxtrt::task<std::string> {
                        auto chunk = co_await source.take_some();
                        if (!chunk)
                            co_return std::string{};
                        co_return std::string{nxtrt::as_string_view(*chunk)};
                    });

                    expect(result == "xy");
                };

                "from byte counts"_test = [] {
                    auto deck = nxtrt::deck{};
                    auto read = [](std::span<std::byte> dst)
                        -> nxtrt::task<std::size_t> {
                        auto text = std::string_view{"xy"};
                        std::memcpy(dst.data(), text.data(), text.size());
                        co_return text.size();
                    };
                    auto storage = std::array<std::byte, 4>{};
                    auto source = nxtrt::task_byte_source{read, std::span{storage}};

                    auto result = deck.sync_wait(
                        [&]() -> nxtrt::task<nxtrt::read_result> {
                        auto chunk = co_await source.take_some();
                        co_return nxtrt::read_result{
                            .bytes = chunk ? chunk->size() : 0,
                            .eof = !chunk,
                        };
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

                auto deck = nxtrt::deck{};
                auto chunks = std::array{"abcd"sv};
                auto storage = std::array<std::byte, 4>{};
                auto reader = text_source(chunks, std::span{storage});

                deck.sync_wait([&]() -> nxtrt::task<void> {
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

                auto deck = nxtrt::deck{};
                auto chunks = std::array{""sv};
                auto storage = std::array<std::byte, 4>{};
                auto reader = text_source(chunks, std::span{storage});

                auto result = deck.sync_wait(
                    [&]() -> nxtrt::task<std::optional<pair>> {
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

                auto deck = nxtrt::deck{};
                auto storage = std::array<std::byte, 8>{};
                auto reader = empty_then_string_source{storage.size()};

                auto result = deck.sync_wait(
                    [&]() -> nxtrt::task<std::optional<pair>> {
                    co_return co_await reader.take_struct<pair>();
                });

                expect(result.has_value());
                expect(result->a == static_cast<unsigned char>('a'));
                expect(result->b == static_cast<unsigned char>('b'));
            };

            "byte_reader takes borrowed string views"_test = [] {
                auto deck = nxtrt::deck{};
                auto chunks = std::array{"abcd"sv};
                auto storage = std::array<std::byte, 4>{};
                auto reader = text_source(chunks, std::span{storage});

                auto result = deck.sync_wait([&]() -> nxtrt::task<std::string> {
                    auto view = co_await reader.take_string_view(3);
                    co_return std::string{view};
                });

                expect(result == "abc");
                expect(reader.buffered_size() == std::size_t{1});
            };

            "byte_reader requires storage for source bytes"_test = [] {
                auto chunks = std::array{"x"sv};
                auto rejected_borrowed = false;
                try {
                    auto reader = text_source(chunks, std::span<std::byte>{});
                    (void)reader;
                } catch (const nxtrt::buffer_error &) {
                    rejected_borrowed = true;
                }

                auto rejected_owned = false;
                try {
                    auto reader = nxtrt::byte_span_source{
                        chunks,
                        std::size_t{0},
                    };
                    (void)reader;
                } catch (const nxtrt::buffer_error &) {
                    rejected_owned = true;
                }

                expect(rejected_borrowed);
                expect(rejected_owned);
            };

            "BYTE WRITER"_test = [] {
                "with borrowed storage"_test = [] {
                    "buffers bytes until flush"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto storage = std::array<std::byte, 4>{};
                        auto writer =
                            chunking_string_sink{64, std::span{storage}};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write(std::string{"ab"});
                            expect(writer.text.empty());
                            co_await writer.write(std::string{"cd"});
                            expect(writer.text.empty());
                            co_await writer.write(std::string{"e"});
                            expect(writer.text == "abcd");
                            co_await writer.flush();
                        });

                        expect(writer.text == "abcde");
                    };

                    "buffer hits do not add suspension"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto storage = std::array<std::byte, 4>{};
                        auto writer =
                            chunking_string_sink{64, std::span{storage}};
                        auto events = std::vector<int>{};

                        auto task = write_three_buffered_bytes(writer, events);
                        deck.start(task);
                        deck.run_ready();

                        expect(task.done())
                            << "three buffered write() calls must finish inline";
                        expect(events == std::vector<int>{1, 2, 3, 4});
                        expect(writer.text.empty());

                        deck.sync_wait(flush_writer(writer));
                        expect(writer.text == "abc");
                    };
                };

                "with owned storage"_test = [] {
                    "buffers bytes until flush"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer = chunking_string_sink{64, std::size_t{4}};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write(std::string{"ab"});
                            expect(writer.text.empty());
                            co_await writer.write(std::string{"cd"});
                            expect(writer.text.empty());
                            co_await writer.flush();
                        });

                        expect(writer.text == "abcd");
                    };

                    "writes ranges of text chunks"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer = chunking_string_sink{64, std::size_t{4}};
                        auto chunks =
                            std::vector<std::string>{"ab", "cd", "e"};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write(chunks);
                            expect(writer.text == "abcd");
                            co_await writer.flush();
                        });

                        expect(writer.text == "abcde");
                    };

                    "writes and flushes text chunks"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer = chunking_string_sink{64, std::size_t{8}};
                        auto chunks =
                            std::vector<std::string>{"ab", "cd", "e"};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write_all(chunks);
                        });

                        expect(writer.text == "abcde");
                        expect(writer.buffered_size() == std::size_t{0});
                    };

                    "drains buffered prefix before direct bytes"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer = chunking_string_sink{3, std::size_t{4}};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write("ab"sv);
                            expect(writer.text.empty());
                            co_await writer.write("cdef"sv);
                        });

                        expect(writer.text == "abcdef");
                        expect(writer.buffered_size() == std::size_t{0});
                    };

                    "writes lazy ranges of text chunks"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer = chunking_string_sink{64, std::size_t{8}};
                        auto numbers = std::views::iota(1, 4);
                        auto chunks = numbers
                            | std::views::transform([](int n) {
                                return std::to_string(n);
                            });

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write(chunks);
                            co_await writer.flush();
                        });

                        expect(writer.text == "123");
                    };

                    "writes ranges of byte spans"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer = chunking_string_sink{64, std::size_t{4}};
                        auto chunks = std::array{
                            nxtrt::as_bytes("ab"sv),
                            nxtrt::as_bytes("cd"sv),
                            nxtrt::as_bytes("ef"sv),
                        };

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write(chunks);
                            co_await writer.flush();
                        });

                        expect(writer.text == "abcdef");
                    };

                    "writes and flushes byte spans"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer = chunking_string_sink{64, std::size_t{8}};
                        auto chunks = std::array{
                            nxtrt::as_bytes("ab"sv),
                            nxtrt::as_bytes("cd"sv),
                            nxtrt::as_bytes("ef"sv),
                        };

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write_all(chunks);
                        });

                        expect(writer.text == "abcdef");
                        expect(writer.buffered_size() == std::size_t{0});
                    };

                    "free write_all borrows lvalue sinks"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto sink = chunking_string_sink{64};
                        auto chunks =
                            std::vector<std::string>{"ab", "cd", "e"};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await nxtrt::write_all(sink, chunks);
                        });

                        expect(sink.text == "abcde");
                    };

                    "free write_all uses explicitly owned sinks"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto text = std::make_shared<std::string>();
                        auto sink = shared_string_sink{text};
                        auto chunks =
                            std::vector<std::string>{"ab", "cd", "e"};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await nxtrt::write_all(sink, chunks);
                        });

                        expect(*text == "abcde");
                    };
                };

                "with borrowed sink and owned storage"_test = [] {
                    "buffers bytes until flush"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto text = std::make_shared<std::string>();
                        auto writer = shared_string_sink{
                            text,
                            std::size_t{64},
                            std::size_t{4},
                        };

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write(std::string{"abc"});
                            expect(text->empty());
                            co_await writer.write(std::string{"de"});
                            expect(*text == "abcd");
                            co_await writer.flush();
                        });

                        expect(*text == "abcde");
                    };
                };

                "with zero storage"_test = [] {
                    "owned zero-size buffers write directly"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer = chunking_string_sink{2, std::size_t{0}};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write("abcde"sv);
                            expect(writer.text == "abcde");
                            expect(writer.buffered_size() == std::size_t{0});
                            co_await writer.flush();
                        });

                        expect(writer.text == "abcde");
                    };

                    "borrowed empty buffers write directly"_test = [] {
                        auto deck = nxtrt::deck{};
                        auto writer =
                            chunking_string_sink{64, std::span<std::byte>{}};

                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.write("ab"sv);
                            expect(writer.text == "ab");
                            expect(writer.buffered_size() == std::size_t{0});
                            co_await writer.flush();
                        });

                        expect(writer.text == "ab");
                    };
                };
            };
        };

        "channels"_test = [] {
            "buffer values until consumed"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = nxtrt::channel<int>{};

                expect(deck.sync_wait(events.publish(1)));
                expect(deck.sync_wait(events.publish(2)));

                auto values = deck.sync_wait([&]() -> nxtrt::task<
                    std::vector<int>> {
                    auto out = std::vector<int>{};
                    out.push_back(*(co_await events.next()));
                    out.push_back(*(co_await events.next()));
                    co_return out;
                });

                expect(values == std::vector<int>{1, 2});
            };

            "resumes a waiting consumer when a value is published"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = nxtrt::channel<int>{};
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
                auto deck = nxtrt::deck{};
                auto events = nxtrt::channel<int>{};

                expect(deck.sync_wait(events.publish(1)));
                events.close();

                auto first = deck.sync_wait(
                    [&]() -> nxtrt::task<std::optional<int>> {
                    co_return co_await events.next();
                });
                auto second = deck.sync_wait(
                    [&]() -> nxtrt::task<std::optional<int>> {
                    co_return co_await events.next();
                });

                expect(first && *first == 1_i);
                expect(!second);
                expect(!deck.sync_wait(events.publish(2)));
            };

            "cancel requests stop and wakes pending consumers"_test = [] {
                auto deck = nxtrt::deck{};
                auto events = nxtrt::channel<int>{};
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
                auto deck = nxtrt::deck{};
                auto events = nxtrt::channel<int>{};

                expect(deck.sync_wait(events.push(3)));
                auto value = events.try_pop();
                expect(value && *value == 3_i);
                expect(!events.try_pop());
            };
        };

        "events"_test = [] {
            "set wakes all waiting tasks"_test = [] {
                auto deck = nxtrt::deck{};
                auto ready = nxtrt::event{};
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
                auto deck = nxtrt::deck{};
                auto ready = nxtrt::event{};
                auto values = std::vector<int>{};

                ready.set();
                deck.sync_wait([&]() -> nxtrt::task<void> {
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
                auto url = nxtrt::http::parse_url(
                    "http://example.test:8080/path?q=1");

                expect(!url.tls);
                expect(url.host == "example.test");
                expect(url.port == "8080");
                expect(url.target == "/path?q=1");
                expect(nxtrt::http::host_header(url)
                       == "example.test:8080");
            };

            "serialize HTTP/1.1 requests"_test = [] {
                auto wire = nxtrt::http::serialize(
                    nxtrt::http::request{
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
                auto deck = nxtrt::deck{};
                auto chunks = std::array{
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"
                    "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"sv,
                };
                auto storage = std::array<std::byte, 256>{};
                auto reader = text_source(chunks, std::span{storage});

                auto result =
                    deck.sync_wait([&]() -> nxtrt::task<std::string> {
                        auto first =
                            co_await nxtrt::http::read_response_head(
                                reader);
                        expect(first.status == 200_i);
                        expect(nxtrt::http::is_chunked(first));

                        auto body = nxtrt::http::read_response_body(
                            reader, first);
                        auto text = std::string{};
                        while (auto chunk = co_await body.next())
                            text += nxtrt::as_string_view(*chunk);

                        auto second =
                            co_await nxtrt::http::read_response_head(
                                reader);
                        expect(second.status == 204_i);
                        expect(
                            nxtrt::http::content_length(second)
                            == std::size_t{0});
                        co_return text;
                    });

                expect(result == "hello world");
            };

            "the next response remains buffered after content-length bodies"_test =
                [] {
                    auto deck = nxtrt::deck{};
                    auto chunks = std::array{
                        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
                        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n"sv,
                    };
                    auto storage = std::array<std::byte, 128>{};
                    auto reader = text_source(chunks, std::span{storage});

                    auto result =
                        deck.sync_wait([&]() -> nxtrt::task<std::string> {
                            auto first =
                                co_await nxtrt::http::read_response_head(
                                    reader);
                            expect(first.status == 200_i);

                            auto body = nxtrt::http::read_response_body(
                                reader, first);
                            auto text = std::string{};
                            while (auto chunk = co_await body.next())
                                text += nxtrt::as_string_view(*chunk);

                            auto second =
                                co_await nxtrt::http::read_response_head(
                                    reader);
                            expect(second.status == 201_i);
                            co_return text;
                        });

                    expect(result == "hello");
                };

            "server-sent events parse through response body readers"_test = [] {
                auto deck = nxtrt::deck{};
                auto chunks = std::array{
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "38\r\n"
                    "event: response.output_text.delta\n"
                    "data: {\"delta\":\"hi\"}\n"
                    "\n"
                    "\r\n"
                    "0\r\n\r\n"sv,
                };
                auto head_storage = std::array<std::byte, 256>{};
                auto reader = text_source(chunks, std::span{head_storage});

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    auto head =
                        co_await nxtrt::http::read_response_head(reader);
                    auto body =
                        nxtrt::http::read_response_body(reader, head);

                    auto event =
                        co_await nxtrt::http::parse_sse_event(body);
                    expect(event.has_value());
                    expect(event->type == "response.output_text.delta");
                    expect(event->data == "{\"delta\":\"hi\"}");

                    auto end =
                        co_await nxtrt::http::parse_sse_event(body);
                    expect(!end);
                });
            };
        };
    }};

} // namespace nxt::test
