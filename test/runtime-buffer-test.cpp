#include "runtime-test.hpp"

namespace nxt::test {

nxtrt::task<void>
send_move_only_wire_value(nxtrt::wire<std::unique_ptr<int>> & channel)
{
    expect(co_await channel.send(std::make_unique<int>(2)));
}

nxtrt::task<std::vector<int>>
receive_move_only_wire_values(nxtrt::wire<std::unique_ptr<int>> & channel)
{
    co_await nxtrt::yield();
    auto first = co_await channel.rx().take_one();
    auto second = co_await channel.rx().take_one();
    co_return std::vector{*first, *second};
}

nxtrt::task<std::vector<int>>
move_only_wire_roundtrip(nxtrt::wire<std::unique_ptr<int>> & channel)
{
    auto work = co_await nxtrt::when_all(
        std::tuple{
            send_move_only_wire_value(channel),
            receive_move_only_wire_values(channel),
        });
    co_return std::get<1>(std::move(work));
}

nxtrt::task<std::string> collect_bytes(nxtrt::bytefeed & reader)
{
    auto text = std::string{};
    while (auto chunk = co_await reader.take_some())
        text += nxtrt::as_string_view(*chunk);
    co_return text;
}

void declare_runtime_buffer_tests()
{
    "tool batches"_group = [] {
        "parse calls and return function_call_output items in order"_test = [] {
            auto deck = nxtrt::deck{};
            auto calls = deck.sync_wait([] {
                return nxtai::tools::read_function_calls_from_items(
                    std::vector{
                        nxtai::openai::raw_json{
                            R"({"id":"fc_1","type":"function_call","call_id":"call_1","name":"echo","arguments":"{\"text\":\"one\"}"})"},
                        nxtai::openai::raw_json{
                            R"({"id":"fc_2","type":"function_call","call_id":"call_2","name":"echo","arguments":"{\"text\":\"two\"}"})"},
                    });
            });
            auto tools = nxtai::tools::make_tool_registry({
                nxtai::tools::make_function_tool(echo_tool{}),
            });

            auto results = deck.sync_wait([&]() mutable {
                return nxtai::tools::run_function_tool_batch(
                    tools,
                    std::move(calls));
            });

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

            auto results = deck.sync_wait([&]() mutable {
                return nxtai::tools::run_function_tool_batch(
                    tools,
                    std::move(calls));
            });

            expect(results.size() == 1_ul);
            expect(results[0].result.failed);
            expect(results[0].result.output == "unknown tool");
        };

        "bounded admission reuses slots and preserves input order"_test =
            [] {
                for (auto capacity : {1, 2, 4, 9}) {
                    auto deck = nxtrt::deck{};
                    auto state = tool_batch_probe{
                        .delays = {80, 1, 5, 1, 3, 2, 1}};
                    auto tools = nxtai::tools::make_tool_registry(
                        {nxtai::tools::make_function_tool(
                            batch_probe_tool{.state = &state})});
                    auto results = deck.sync_wait([&] {
                        if (capacity == 4)
                            return nxtai::tools::
                                run_function_tool_batch(
                                    tools, tool_batch_probe_calls(7));
                        return nxtai::tools::run_function_tool_batch(
                            tools, tool_batch_probe_calls(7), capacity);
                    });
                    expect(state.peak == std::min(capacity, 7));
                    expect(state.started == 7 && state.settled == 7);
                    expect(state.active == 0);
                    expect(
                        state.completed.front()
                        == (capacity == 1 ? 0 : 1));
                    expect(results.size() == 7_ul);
                    for (int i = 0; i < 7; ++i) {
                        expect(
                            results[i].call.call_id
                            == std::to_string(i));
                        expect(
                            results[i].result.output
                            == std::to_string(i));
                        expect(!results[i].result.failed);
                    }
                }
            };

        "tool ideas feed completion order directly through a pool"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto state = tool_batch_probe{.delays = {80, 1, 1}};
                auto tools = nxtai::tools::make_tool_registry(
                    {nxtai::tools::make_function_tool(
                        batch_probe_tool{.state = &state})});
                auto calls = tool_batch_probe_calls(3);
                using idea = nxtai::tools::function_call_idea;
                auto ideas =
                    calls | std::views::transform([&](auto & call) {
                        return idea{&tools, std::move(call)};
                    });
                auto input = nxtrt::value_range_source{ideas};
                auto slots = std::array<nxtrt::pool_slot<idea>, 2>{};
                auto available =
                    nxtrt::farm<nxtrt::pool_slot<idea>, 2>{&slots};
                auto output = nxtrt::static_value_storage<
                    nxtai::tools::function_call_result,
                    2>{};
                auto results =
                    nxtrt::pool<idea>{input, available, output.ref()};
                auto collected =
                    std::vector<nxtai::tools::function_call_result>{};
                auto sink = nxtrt::container_sink{collected};
                auto count = deck.sync_wait([&] {
                    return nxtrt::finally(
                        nxtrt::stream_all(results, sink),
                        [&] { return results.close(); });
                });
                expect(count == 3_ul);
                expect(collected.size() == 3_ul);
                expect(collected[0].call.call_id == "1");
                expect(collected[1].call.call_id == "2");
                expect(collected[2].call.call_id == "0");
                expect(collected[0].result.output == "1");
                expect(state.peak == 2 && state.active == 0);
                expect(results.occupied() == 0_ul);
            };

        "tool failures and invalid arguments remain per call results"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto state = tool_batch_probe{
                    .delays = {1, 1, 1}, .failures = {0}};
                auto tools = nxtai::tools::make_tool_registry(
                    {nxtai::tools::make_function_tool(
                        batch_probe_tool{.state = &state})});
                auto calls = tool_batch_probe_calls(3);
                calls[1].arguments = R"({"missing":"text"})";
                auto results = deck.sync_wait([&] {
                    return nxtai::tools::run_function_tool_batch(
                        tools, std::move(calls), 1);
                });
                expect(results[0].result.failed);
                expect(
                    results[0].result.output
                    == "tool execution failed: probe failure 0");
                expect(results[1].result.failed);
                expect(
                    results[1].result.output
                    == "invalid tool arguments json");
                expect(!results[2].result.failed);
                expect(results[2].result.output == "2");
                expect(state.started == 2 && state.settled == 2);
            };

        "typed tool cancellation is not converted to a failed result"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto state = tool_batch_probe{
                    .delays = std::vector<int>(7, 1),
                    .self_cancel = true};
                auto tools = nxtai::tools::make_tool_registry(
                    {nxtai::tools::make_function_tool(
                        batch_probe_tool{.state = &state})});
                auto cancelled = false;
                try {
                    (void)deck.sync_wait([&] {
                        return nxtai::tools::run_function_tool_batch(
                            tools, tool_batch_probe_calls(7), 1);
                    });
                } catch (const nxtrt::operation_cancelled &) {
                    cancelled = true;
                }
                expect(cancelled);
                expect(state.started == 1 && state.settled == 1);
                expect(state.active == 0 && state.cancelled == 1);
            };

        "infrastructure failures settle all calls before input order rethrow"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto state = tool_batch_probe{
                    .delays = {80, 1, 1, 1, 1}, .failures = {0, 1}};
                // Bypass the typed tool adapter so exceptions are
                // transport / registry failures rather than ordinary
                // failed tool results.
                auto tools = nxtai::tools::make_tool_registry(
                    {nxtai::tools::function_tool_entry{
                        .name = "echo",
                        .description = {},
                        .parameters = {},
                        .run = [&state](std::string_view arguments) {
                            auto text =
                                nxtai::tools::json_string_member(
                                    arguments, "text");
                            return run_tool_batch_probe(
                                state, std::stoi(*text));
                        }}});
                auto message = std::string{};
                try {
                    (void)deck.sync_wait([&] {
                        return nxtai::tools::run_function_tool_batch(
                            tools, tool_batch_probe_calls(5), 2);
                    });
                } catch (const nxtrt::runtime_error & error) {
                    message = error.what();
                }
                expect(message == "probe failure 0");
                expect(state.completed.front() == 1);
                expect(state.started == 5 && state.settled == 5);
                expect(state.active == 0);
            };

        "cancellation at startup and during work stops admission and drains"_test =
            [] {
                auto saw_running = false;
                for (int turns = 0; turns < 30; ++turns) {
                    auto deck = nxtrt::deck{};
                    auto state = tool_batch_probe{.block = true};
                    auto tools = nxtai::tools::make_tool_registry(
                        {nxtai::tools::make_function_tool(
                            batch_probe_tool{.state = &state})});
                    auto root = nxtrt::root_task{
                        deck, [&] {
                            return nxtai::tools::
                                run_function_tool_batch(
                                    tools,
                                    tool_batch_probe_calls(9),
                                    2);
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
                    expect(state.started <= 2);
                    expect(state.active == 0);
                    expect(state.settled == state.started);
                    expect(state.cancelled == state.started);
                    if (turns == 0)
                        expect(state.started == 0);
                    saw_running |= state.started != 0;
                }
                expect(saw_running);
            };

        "empty batches and invalid capacity do not invoke tools"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto tools = nxtai::tools::tool_registry{};
                auto results = deck.sync_wait([&] {
                    return nxtai::tools::run_function_tool_batch(
                        tools, {});
                });
                expect(results.empty());
                auto rejected = false;
                try {
                    (void)deck.sync_wait([&] {
                        return nxtai::tools::run_function_tool_batch(
                            tools, {}, 0);
                    });
                } catch (const std::invalid_argument &) {
                    rejected = true;
                }
                expect(rejected);
            };
    };

    "wishes"_group = [] {
        "typed urges are prepared and parked"_test = [] {
            auto wand = manual_wand{};
            auto deck = nxtrt::deck{&wand};
            auto events = std::vector<int>{};

            auto task_body =
                [](std::vector<int> & events) -> nxtrt::task<void> {
                events.push_back(1);
                co_await nxtrt::op::manual{42};
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
                << "task should suspend before manual wish fulfillment";
            expect(deck.empty())
                << "manual wish should not requeue itself";
            expect(
                wand.prepared == std::vector<nxtrt::coin_t>{42})
                << "wand should synchronously prepare the wish";
            expect(wand.parked.size() == std::size_t{1})
                << "urge should park the suspended coroutine";
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
                co_await nxtrt::op::manual{7};
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

            expect(events == std::vector<int>{1});
            expect(
                wand.prepared == std::vector<nxtrt::coin_t>{7});
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

            auto root = nxtrt::root_task{
                deck,
                []() -> nxtrt::task<void> {
                    co_await nxtrt::op::manual{99};
                },
            };

            root.start();
            deck.run_ready();
            root.inner().request_stop();

            expect(wand.cancelled == std::vector<nxtrt::coin_t>{99});
        };
    };

    "buffers"_group = [] {
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

        "ring regions expose constructed chunks and raw capacity"_test = [] {
            auto storage = std::array<std::byte, 4>{};
            auto ring = nxtrt::ring_region<std::byte>{
                storage.data(),
                storage.size(),
            };

            auto dst = ring.unconstructed_capacity();
            expect(dst.size() == std::size_t{4});
            auto text = std::string_view{"abc"};
            std::memcpy(
                dst.as_writable_bytes().data(),
                text.data(),
                text.size());
            ring.advance_constructed(text.size());

            auto chunks = ring.constructed();
            expect(chunks.chunk_count() == std::size_t{1});
            expect(nxtrt::as_string_view(chunks.chunks().front()) == "abc");

            ring.destroy_all();
            expect(ring.empty());
            expect(ring.unused_capacity_size() == std::size_t{4});
        };

        "wire feed consumption returns capacity for move-only values"_test =
            [] {
                using value = std::unique_ptr<int>;
                auto deck = nxtrt::deck{};
                auto storage = nxtrt::rack<value>{1};
                auto channel = nxtrt::wire<value>{storage};
                expect(channel.try_send(std::make_unique<int>(1)));

                auto values = deck.sync_wait(
                    [&] { return move_only_wire_roundtrip(channel); });

                expect(values == std::vector<int>{1, 2});
            };

        "rebases compact wrapped free space without draining preserved values"_test =
            []() -> nxtrt::task<void> {
            auto writer = chunking_string_sink{2, std::size_t{6}};
            co_await nxtrt::write(writer, "abcde"sv);
            co_await writer.rebase(3, 2);
            expect(writer.text == "ab");
            expect(byte_value_chunks_text(writer.buffered()) == "cde");
            expect(writer.unused_capacity().size() >= std::size_t{2});
        };

        "reel retries zero-progress non-EOF discards"_test = [] {
            class intermittent_feed final : public nxtrt::bytefeed
            {
            public:
                intermittent_feed()
                    : nxtrt::bytefeed(std::size_t{3})
                {
                }

            private:
                nxtrt::hope<nxtrt::fare_t>
                discard_more(std::size_t limit) override
                {
                    if (calls_++ == 0)
                        return nxtrt::hope<nxtrt::fare_t>::ready(0);
                    return nxtrt::hope<nxtrt::fare_t>::ready(
                        std::min(limit, std::size_t{3}));
                }

                int calls_ = 0;
            };

            auto deck = nxtrt::deck{};
            auto source = intermittent_feed{};
            auto frames =
                nxtrt::reel<std::byte, counted_byte_frame>{source};
            deck.sync_wait([&]() -> nxtrt::task<void> {
                co_await frames.discard_prefix(3);
            });
        };

        "ring regions preserve wrapped constructed values"_test = [] {
            auto storage = nxtrt::static_value_storage<int, 3>{};
            auto ring = nxtrt::ring_region<int>{
                storage.data(),
                storage.size(),
            };

            for (auto value : {1, 2, 3}) {
                std::construct_at(
                    ring.data() + ring.write_index(),
                    value);
                ring.advance_constructed(1);
            }

            ring.destroy_prefix(2);
            for (auto value : {4, 5}) {
                std::construct_at(
                    ring.data() + ring.write_index(),
                    value);
                ring.advance_constructed(1);
            }

            auto chunks = ring.constructed();
            expect(chunks.size() == std::size_t{3});
            expect(chunks.chunk_count() == std::size_t{2});
            expect(chunks.chunks()[0].size() == std::size_t{1});
            expect(chunks.chunks()[0][0] == 3_i);
            expect(chunks.chunks()[1].size() == std::size_t{2});
            expect(chunks.chunks()[1][0] == 4_i);
            expect(chunks.chunks()[1][1] == 5_i);

            ring.destroy_all();
        };

        "masks size their summary tree by powers of 64"_test = [] {
            expect(nxtrt::mask<>::words_for(1) == std::size_t{1});
            expect(nxtrt::mask<>::words_for(64) == std::size_t{1});
            expect(nxtrt::mask<>::words_for(65) == std::size_t{3});
            expect(nxtrt::mask<>::words_for(4096) == std::size_t{65});
            expect(nxtrt::mask<>::words_for(4097) == std::size_t{68});
            expect(nxtrt::mask<4096>::words == std::size_t{65});
        };

        "masks hand out their lowest member first"_test = [] {
            auto bits = nxtrt::mask<4096>{};
            expect(bits.empty());
            expect(bits.take() == std::size_t{4096});
            for (auto index : {4095uz, 64uz, 3uz, 63uz, 1000uz})
                bits.give(index);
            for (auto index : {3uz, 63uz, 64uz, 1000uz, 4095uz}) {
                expect(bits.contains(index));
                expect(bits.take() == index);
                expect(!bits.contains(index));
            }
            expect(bits.empty());
        };

        "masks agree with a set under random takes and gives"_test = [] {
            for (auto capacity : {1uz, 5uz, 63uz, 64uz, 65uz, 200uz,
                                  4095uz, 4096uz, 4097uz, 300000uz}) {
                auto words = std::vector<std::uint64_t>(
                    nxtrt::mask<>::words_for(capacity));
                auto bits = nxtrt::mask<>{words, capacity};
                auto model = std::set<std::size_t>{};
                auto state = std::uint64_t{capacity * 2654435761u + 1};
                auto next = [&] {
                    state = state * 6364136223846793005u
                        + 1442695040888963407u;
                    return static_cast<std::size_t>(state >> 33);
                };

                if (capacity <= 4097) {
                    bits.fill();
                    for (auto i = 0uz; i < capacity; ++i)
                        model.insert(i);
                }
                for (auto step = 0; step < 4000; ++step) {
                    if (next() % 3 == 0) {
                        auto expected = model.empty()
                            ? capacity
                            : *model.begin();
                        expect(bits.take() == expected);
                        if (!model.empty())
                            model.erase(model.begin());
                    } else {
                        auto index = next() % capacity;
                        if (!model.contains(index)) {
                            bits.give(index);
                            model.insert(index);
                        }
                    }
                    expect(bits.empty() == model.empty());
                }
            }
        };

        "runtime-sized farms hand out every slot once"_test = [] {
            auto values = std::vector<int>(300);
            auto hot = std::vector<std::size_t>(
                nxtrt::farm<int>::hot_capacity_for(values.size()));
            auto cold = std::vector<std::uint64_t>(
                nxtrt::mask<>::words_for(values.size()));
            auto farm = nxtrt::farm<int>{values, {hot, cold}};
            expect(farm.capacity() == std::size_t{300});

            auto seen = std::set<int *>{};
            for (auto i = 0uz; i < values.size(); ++i) {
                auto * slot = farm.alloc().take_ready();
                expect(slot != nullptr);
                expect(slot == values.data() + i);
                seen.insert(slot);
            }
            expect(seen.size() == values.size());
            expect(farm.empty());
            expect(farm.alloc().take_ready() == nullptr);

            farm.release(values.data() + 7);
            farm.release(values.data() + 299);
            expect(farm.alloc().take_ready() == values.data() + 7);
            expect(farm.alloc().take_ready() == values.data() + 299);
            expect(farm.alloc().take_ready() == nullptr);
        };

        "farms cache free slots over a mask"_test = [] {
            auto values = std::array<int, 128>{};
            auto farm = nxtrt::farm<int, values.size()>{&values};
            auto & indices = static_cast<nxtrt::feed<std::size_t> &>(farm);

            auto first = indices.take();
            expect(first.is_ready());
            auto first_index = first.take_ready();
            expect(first_index.has_value());
            expect(*first_index == std::size_t{0});

            auto second = farm.alloc();
            expect(second.is_ready());
            auto * second_slot = second.take_ready();
            expect(second_slot != nullptr);
            expect(second_slot == values.data() + 1);
            *second_slot = 42;
            expect(*second_slot == 42_i);

            farm.give(*first_index);
            auto next = farm.take();
            expect(next.is_ready());
            auto next_index = next.take_ready();
            expect(next_index.has_value());
            expect(*next_index == std::size_t{2});

            farm.release(second_slot);
            auto saw_released = false;
            for (auto i = std::size_t{0}; i < 64 && !saw_released; ++i)
                saw_released = farm.alloc().take_ready() == second_slot;
            expect(saw_released);
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

        "byte span feed accepts lazy ranges"_test = [] {
            auto deck = nxtrt::deck{};
            auto texts = std::array{"ab"sv, ""sv, "cde"sv, "f"sv};
            auto storage = std::array<std::byte, 3>{};
            auto source = nxtrt::byte_span_feed{
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

        "gzip feed accepts borrowed output storage"_test = [] {
            auto deck = nxtrt::deck{};
            auto plain = "hello borrowed gzip buffer"sv;
            auto compressed = gzip_text(plain);
            auto chunks = std::array{std::string_view{compressed}};
            auto source_storage = std::array<std::byte, 8>{};
            auto inflate_storage = std::array<std::byte, 5>{};
            auto source = text_source(chunks, std::span{source_storage});
            auto reader =
                nxtrt::gzip_reader(source, std::span{inflate_storage});

            auto result =
                deck.sync_wait([&]() -> nxtrt::task<std::string> {
                    auto text = std::string{};
                    while (auto chunk = co_await reader.take_some())
                        text += nxtrt::as_string_view(*chunk);
                    co_return text;
                });

            expect(result == plain);
        };

        "gzip reader leaves bytes after its stream in the source"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto compressed = gzip_text("body");
                compressed += "TAIL";
                auto chunks = std::array{std::string_view{compressed}};
                auto source_storage = std::array<std::byte, 32>{};
                auto output_storage = std::array<std::byte, 8>{};
                auto source =
                    text_source(chunks, std::span{source_storage});
                auto reader =
                    nxtrt::gzip_reader(source, std::span{output_storage});

                auto result =
                    deck.sync_wait([&] { return collect_bytes(reader); });
                expect(result == "body");
                expect(
                    nxtrt::as_string_view(source.buffered_span())
                    == "TAIL");
            };

#if defined(NXTRT_HAVE_ZSTD)
        "zstd reader concatenates frames and preserves trailing source bytes"_test =
            [] {
                for (auto capacity : {4, 64}) {
                    for (auto tail : {"TAIL"sv, "!"sv, ""sv}) {
                        auto deck = nxtrt::deck{};
                        auto skippable = std::string{
                            "\x50\x2a\x4d\x18\x03\x00\x00\x00XYZ", 11};
                        auto compressed = zstd_text("first") + skippable
                                          + zstd_text("second")
                                          + std::string{tail};
                        auto chunks =
                            std::array{std::string_view{compressed}};
                        auto source_storage = std::array<std::byte, 64>{};
                        auto output_storage = std::array<std::byte, 5>{};
                        auto source = text_source(
                            chunks,
                            std::span{source_storage}.first(capacity));
                        auto reader = nxtrt::zstd_reader_for(
                            source, std::span{output_storage});

                        auto result = deck.sync_wait(
                            [&] { return collect_bytes(reader); });
                        expect(result == "firstsecond");
                        expect(
                            nxtrt::as_string_view(source.buffered_span())
                            == tail);
                    }
                }
            };
#endif

        "protocol leftovers remain buffered"_test = []() -> nxtrt::task<void> {
            auto chunks = std::array{"abc--def--ghi"sv};
            auto storage = std::array<std::byte, 16>{};
            auto reader = text_source(chunks, std::span{storage});

            auto out = std::vector<std::string>{};
            out.emplace_back(
                nxtrt::as_string_view(
                    co_await reader.take_until("--")));
            out.emplace_back(
                nxtrt::as_string_view(
                    co_await reader.take_until("--")));
            out.emplace_back(
                nxtrt::as_string_view(reader.buffered_span()));
            std::vector<std::string> parts = out;

            expect(
                parts == std::vector<std::string>{"abc", "def", "ghi"});
        };

        "buffer hits do not add suspension"_test = [] {
            auto deck = nxtrt::deck{};
            auto chunks = std::array{"abc"sv};
            auto storage = std::array<std::byte, 3>{};
            auto reader = text_source(chunks, std::span{storage});
            auto events = std::vector<int>{};

            auto root = nxtrt::root_task{
                deck,
                [&] {
                    return take_three_buffered_bytes(reader, events);
                },
            };
            root.start();

            deck.run_ready();
            expect(!root.inner().done())
                << "initial fill should delegate to a slow task";
            deck.run_ready();
            expect(!root.inner().done())
                << "fill continuation should wait for the next deck turn";
            deck.run_ready();

            expect(root.inner().done())
                << "three buffered take(1) calls must finish inline";
            expect(events == std::vector<int>{1, 2, 3, 4});
            expect(std::move(root.inner()).result() == "abc");
        };

        "bytefeed peeks through shared chunk views"_test = []() -> nxtrt::task<void> {
            auto chunks = std::array{"abcd"sv};
            auto storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{storage});

            co_await check_bytefeed_chunk_peek(reader);
        };

        "chop lazily scans visible byte chunks"_test = [] {
            auto bytes = std::array{
                byte_value(2),
                byte_value('a'),
                byte_value('b'),
                byte_value(1),
                byte_value('c'),
                byte_value(2),
                byte_value('d'),
            };
            auto all = std::span<const std::byte>{
                bytes.data(),
                bytes.size(),
            };
            auto spans = std::array{
                all.first(4),
                all.subspan(4),
            };
            auto chunks = nxtrt::byte_chunks<const std::byte>{
                std::span{spans},
            };

            auto frames = nxtrt::chop<std::byte, counted_byte_frame>(
                chunks);

            expect(frames.count() == std::size_t{2});
            expect(frames.extent() == std::size_t{5});
            expect(
                counted_payload_texts(frames | std::views::take(2))
                == std::vector<std::string>{"ab", "c"});
            expect(
                nxtrt::chop_extent(frames | std::views::take(2))
                == std::size_t{5});
        };

        "reel peeks chops from a bytefeed without storing frames"_test = []() -> nxtrt::task<void> {
            auto bytes = std::array{
                byte_value(2),
                byte_value('a'),
                byte_value('b'),
                byte_value(1),
                byte_value('c'),
                byte_value(2),
                byte_value('d'),
                byte_value('e'),
            };
            auto all = std::span<const std::byte>{
                bytes.data(),
                bytes.size(),
            };
            auto chunks = std::array{
                all,
            };
            auto storage = std::array<std::byte, 5>{};
            auto reader = nxtrt::byte_span_feed{
                chunks,
                std::span{storage},
            };

            co_await check_reel_chops_ring_feed(reader);
        };

        "reel is generic over source stock values"_test = []() -> nxtrt::task<void> {
            auto source = int_feed{
                std::vector<int>{2, 10, 20, 1, 30, 2, 40, 50},
                5,
            };

            co_await check_stock_reel_chops_ring_feed(source);
        };

        "empty reads are distinguished from EOF"_test = []() -> nxtrt::task<void> {
            auto storage = std::array<std::byte, 8>{};
            auto reader = empty_then_string_source{storage.size()};

            auto out = std::vector<std::string>{};
            while (auto chunk = co_await reader.take_some())
                out.emplace_back(
                    nxtrt::as_string_view(*chunk));
            std::vector<std::string> parts = out;

            expect(parts == std::vector<std::string>{"", "abc"});
        };

        "bytefeed streams one chunk into a sink"_test = [] {
            auto deck = nxtrt::deck{};
            auto chunks = std::array{"abcdef"sv};
            auto source_storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{source_storage});
            auto writer = chunking_string_sink{64, std::size_t{0}};

            auto streamed =
                deck.sync_wait([&]() -> nxtrt::task<std::size_t> {
                co_return nxtrt::value_count(
                    co_await reader.stream(writer, 3));
            });

            expect(streamed == std::size_t{3});
            expect(writer.text == "abc");
            expect(reader.buffered_size() == std::size_t{0});
        };

        "bytefeed streams all chunks into a sink"_test = [] {
            auto deck = nxtrt::deck{};
            auto chunks = std::array{"ab"sv, "cde"sv, "f"sv};
            auto source_storage = std::array<std::byte, 2>{};
            auto reader = text_source(chunks, std::span{source_storage});
            auto writer = chunking_string_sink{3, std::size_t{0}};

            auto streamed =
                deck.sync_wait([&]() -> nxtrt::task<std::size_t> {
                    co_return co_await nxtrt::stream_all(reader, writer);
                });

            expect(streamed == std::size_t{6});
            expect(writer.text == "abcdef");
        };

        "bytefeed reads directly into caller storage"_test = []() -> nxtrt::task<void> {
            auto chunks = std::array{"ab"sv, "cde"sv, "fg"sv};
            auto source_storage = std::array<std::byte, 2>{};
            auto reader = text_source(chunks, std::span{source_storage});
            auto out = std::array<std::byte, 5>{};
            auto dsts = std::array{std::span<std::byte>{out}};

            std::size_t read = nxtrt::value_count(
                co_await reader.read_vec(std::span{dsts}));

            expect(read == std::size_t{5});
            expect(nxtrt::as_string_view(out) == "abcde");
            expect(reader.buffered_size() == std::size_t{0});
        };

        "bytefeed read_vec scatters buffered bytes"_test = []() -> nxtrt::task<void> {
            auto chunks = std::array{"abcd"sv};
            auto source_storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{source_storage});
            auto first = std::array<std::byte, 2>{};
            auto second = std::array<std::byte, 1>{};
            auto dsts = std::array{
                std::span<std::byte>{first},
                std::span<std::byte>{second},
            };

            co_await reader.fill(4);
            std::size_t read = nxtrt::value_count(
                co_await reader.read_vec(std::span{dsts}));

            expect(read == std::size_t{3});
            expect(nxtrt::as_string_view(first) == "ab");
            expect(nxtrt::as_string_view(second) == "c");
            expect(nxtrt::as_string_view(reader.buffered_span()) == "d");
        };

        "bytefeed discards without exposing bytes"_test = [] {
            auto deck = nxtrt::deck{};
            auto chunks = std::array{"abcd"sv};
            auto storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{storage});

            auto discarded =
                deck.sync_wait([&]() -> nxtrt::task<std::size_t> {
                co_return nxtrt::value_count(
                    co_await reader.discard(2));
            });
            auto rest = deck.sync_wait([&]() -> nxtrt::task<std::string> {
                co_return std::string{
                    nxtrt::as_string_view(co_await reader.take(2))};
            });
            auto eof = deck.sync_wait([&]() -> nxtrt::task<nxtrt::fare_t> {
                co_return co_await reader.discard();
            });

            expect(discarded == std::size_t{2});
            expect(rest == "cd");
            expect(value_count(eof) == std::size_t{0});
            expect(is_eof(eof));
        };

        "write_all drains borrowed bytes into sinks"_test = []() -> nxtrt::task<void> {
            auto sink = chunking_string_sink{2};

            co_await nxtrt::write_all(
                sink,
                std::string{"abcdef"});

            expect(sink.text == "abcdef");
        };

        "task_bytefeed reads through a task callable"_group = [] {
            "from read results"_test = [] {
                auto deck = nxtrt::deck{};
                auto read = [](nxtrt::junk<std::byte> dst)
                    -> nxtrt::task<nxtrt::fare_t> {
                    auto text = std::string_view{"xy"};
                    std::memcpy(
                        dst.as_writable_bytes().data(),
                        text.data(),
                        text.size());
                    co_return text.size();
                };
                auto storage = std::array<std::byte, 4>{};
                auto source = nxtrt::task_bytefeed{read, std::span{storage}};

                auto result = deck.sync_wait(
                    [&]() -> nxtrt::task<std::string> {
                    auto chunk = co_await source.take_some();
                    if (!chunk)
                        co_return std::string{};
                    auto text = nxtrt::as_string_view(*chunk);
                    co_return std::string{text.data(), text.size()};
                });

                expect(result == "xy");
            };

            "from byte counts"_test = [] {
                auto deck = nxtrt::deck{};
                auto read = [](nxtrt::junk<std::byte> dst)
                    -> nxtrt::task<std::size_t> {
                    auto text = std::string_view{"xy"};
                    std::memcpy(
                        dst.as_writable_bytes().data(),
                        text.data(),
                        text.size());
                    co_return text.size();
                };
                auto storage = std::array<std::byte, 4>{};
                auto source = nxtrt::task_bytefeed{read, std::span{storage}};

                auto result = deck.sync_wait(
                    [&]() -> nxtrt::task<nxtrt::fare_t> {
                    auto chunk = co_await source.take_some();
                    if (!chunk)
                        co_return nxtrt::eof;
                    co_return chunk->size();
                });

                expect(nxtrt::value_count(result) == std::size_t{2});
                expect(!nxtrt::is_eof(result));
            };

            "buffers into its feed storage when the stream sink has no room"_test = []() -> nxtrt::task<void> {
                struct read_once
                {
                    nxtrt::task<nxtrt::fare_t>
                    operator()(nxtrt::junk<std::byte> dst)
                    {
                        auto text = std::string_view{"abc"};
                        auto n = std::min(dst.size(), text.size());
                        std::memcpy(
                            dst.as_writable_bytes().data(),
                            text.data(),
                            n);
                        if (n == 0)
                            co_return nxtrt::eof;
                        co_return n;
                    }
                };

                auto storage = std::array<std::byte, 4>{};
                auto source = nxtrt::task_bytefeed{
                    read_once{},
                    std::span{storage},
                };
                auto writer = chunking_string_sink{64, std::size_t{0}};

                auto first = co_await source.stream(writer, 3);
                expect(nxtrt::value_count(first) == std::size_t{0});
                expect(!nxtrt::is_eof(first));
                expect(writer.text.empty());
                expect(nxtrt::as_string_view(source.buffered_span()) == "abc");

                auto second = co_await source.stream(writer, 3);
                expect(nxtrt::value_count(second) == std::size_t{3});

                expect(writer.text == "abc");
                expect(source.buffered_size() == std::size_t{0});
            };
        };

        "bytefeed peeks and takes copied structs"_test = []() -> nxtrt::task<void> {
            struct pair
            {
                unsigned char a = 0;
                unsigned char b = 0;
            };

            auto chunks = std::array{"abcd"sv};
            auto storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{storage});

            auto first = co_await reader.peek_struct<pair>();
            expect(first.a == static_cast<unsigned char>('a'));
            expect(first.b == static_cast<unsigned char>('b'));
            expect(reader.buffered_size() == std::size_t{4});

            auto second = co_await reader.take_struct<pair>();
            expect(second.has_value());
            expect(second->a == static_cast<unsigned char>('a'));
            expect(second->b == static_cast<unsigned char>('b'));
            expect(reader.buffered_size() == std::size_t{2});
        };

        "bytefeed returns nullopt when taking structs at eof"_test = []() -> nxtrt::task<void> {
            struct pair
            {
                unsigned char a = 0;
                unsigned char b = 0;
            };

            auto chunks = std::array{""sv};
            auto storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{storage});

            std::optional<pair> result = co_await reader.take_struct<pair>();

            expect(!result);
        };

        "bytefeed does not treat empty reads as struct eof"_test = []() -> nxtrt::task<void> {
            struct pair
            {
                unsigned char a = 0;
                unsigned char b = 0;
            };

            auto storage = std::array<std::byte, 8>{};
            auto reader = empty_then_string_source{storage.size()};

            std::optional<pair> result = co_await reader.take_struct<pair>();

            expect(result.has_value());
            expect(result->a == static_cast<unsigned char>('a'));
            expect(result->b == static_cast<unsigned char>('b'));
        };

        "bytefeed takes borrowed string views"_test = []() -> nxtrt::task<void> {
            auto chunks = std::array{"abcd"sv};
            auto storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{storage});

            auto view = co_await reader.take_string_view(3);
            std::string result = std::string{view};

            expect(result == "abc");
            expect(reader.buffered_size() == std::size_t{1});
        };

        "zero-storage bytefeeds stream direct bytes"_test = []() -> nxtrt::task<void> {
            auto chunks = std::array{"xy"sv};
            auto reader = text_source(chunks, std::span<std::byte>{});
            auto sink = chunking_string_sink{64};

            auto streamed = co_await nxtrt::stream_all(reader, sink);

            expect(streamed == std::size_t{2});
            expect(sink.text == "xy");
        };

        "BYTESINK"_group = [] {
            "with borrowed storage"_group = [] {
                "buffers bytes until flush"_test = []() -> nxtrt::task<void> {
                    auto storage = std::array<std::byte, 4>{};
                    auto writer =
                        chunking_string_sink{64, std::span{storage}};

                    co_await nxtrt::write(writer, std::string{"ab"});
                    expect(writer.text.empty());
                    co_await nxtrt::write(writer, std::string{"cd"});
                    expect(writer.text.empty());
                    co_await nxtrt::write(writer, std::string{"e"});
                    expect(writer.text == "abcd");
                    co_await writer.flush();

                    expect(writer.text == "abcde");
                };

                "buffer hits do not add suspension"_test = [] {
                    auto deck = nxtrt::deck{};
                    auto storage = std::array<std::byte, 4>{};
                    auto writer =
                        chunking_string_sink{64, std::span{storage}};
                    auto events = std::vector<int>{};

                    auto root = nxtrt::root_task{
                        deck,
                        [&] {
                            return write_three_buffered_bytes(
                                writer,
                                events);
                        },
                    };
                    root.start();
                    deck.run_ready();

                    expect(root.inner().done())
                        << "three buffered write() calls must finish inline";
                    expect(events == std::vector<int>{1, 2, 3, 4});
                    expect(writer.text.empty());

                    deck.sync_wait([&] {
                        return flush_writer(writer);
                    });
                    expect(writer.text == "abc");
                };
            };

            "with owned storage"_group = [] {
                "buffers bytes until flush"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{4}};

                    co_await nxtrt::write(writer, std::string{"ab"});
                    expect(writer.text.empty());
                    co_await nxtrt::write(writer, std::string{"cd"});
                    expect(writer.text.empty());
                    co_await writer.flush();

                    expect(writer.text == "abcd");
                };

                "writes ranges of text chunks"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{4}};
                    auto chunks =
                        std::vector<std::string>{"ab", "cd", "e"};

                    co_await nxtrt::write(writer, chunks);
                    expect(writer.text == "abcd");
                    co_await writer.flush();

                    expect(writer.text == "abcde");
                };

                "writes and flushes text chunks"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{8}};
                    auto chunks =
                        std::vector<std::string>{"ab", "cd", "e"};

                    co_await nxtrt::write_all(writer, chunks);

                    expect(writer.text == "abcde");
                    expect(writer.buffered_size() == std::size_t{0});
                };

                "writes and flushes string literals"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{8}};

                    co_await nxtrt::write_all(writer, "hello");

                    expect(writer.text == "hello");
                    expect(writer.buffered_size() == std::size_t{0});
                };

                "prints formatted text"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{16}};

                    co_await nxtrt::print(writer, "{}={:02}", "n", 7);
                    expect(writer.text.empty());
                    co_await writer.flush();

                    expect(writer.text == "n=07");
                };

                "prints and flushes formatted text"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{16}};

                    co_await nxtrt::print_all(writer, "{} {}", "hello", 42);

                    expect(writer.text == "hello 42");
                    expect(writer.buffered_size() == std::size_t{0});
                };

                "drains buffered prefix before direct bytes"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{3, std::size_t{4}};

                    co_await nxtrt::write(writer, "ab"sv);
                    expect(writer.text.empty());
                    co_await nxtrt::write(writer, "cdef"sv);

                    expect(writer.text == "abcdef");
                    expect(writer.buffered_size() == std::size_t{0});
                };

                "writes repeated byte patterns"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{8}};

                    co_await nxtrt::write_splat(writer, "ab"sv, 3);
                    expect(writer.text.empty());
                    co_await writer.flush();

                    expect(writer.text == "ababab");
                };

                "drains splatted patterns after buffered prefix"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{4, std::size_t{2}};

                    co_await nxtrt::write(writer, "x"sv);
                    co_await nxtrt::write_splat(writer, "ab"sv, 3);

                    expect(writer.text == "xababab");
                    expect(writer.buffered_size() == std::size_t{0});
                };

                "rebases while preserving recent buffered bytes"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{2, std::size_t{6}};

                    co_await nxtrt::write(writer, "abcdef"sv);
                    co_await writer.rebase(2, 3);
                    expect(writer.text == "abcd");
                    expect(byte_value_chunks_text(writer.buffered()) == "ef");
                    expect(writer.unused_capacity().size() >= std::size_t{3});

                    co_await nxtrt::write(writer, "XYZ"sv);
                    co_await writer.flush();

                    expect(writer.text == "abcdefXYZ");
                };

                "reserves writable slices while preserving recent bytes"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{2, std::size_t{6}};

                    co_await nxtrt::write(writer, "abcdef"sv);
                    auto out =
                        co_await writer.writable_slice_preserve(2, 3);
                    std::memcpy(out.data(), "XYZ", out.size());
                    expect(writer.text == "abcd");
                    expect(
                        byte_value_chunks_text(writer.buffered())
                        == "efXYZ");
                    co_await writer.flush();

                    expect(writer.text == "abcdefXYZ");
                };

                "rejects impossible preserved writable capacity"_test = [] {
                    auto deck = nxtrt::deck{};
                    auto writer = chunking_string_sink{64, std::size_t{4}};

                    auto rejected = false;
                    try {
                        deck.sync_wait([&]() -> nxtrt::task<void> {
                            co_await writer.rebase(3, 2);
                        });
                    } catch (const nxtrt::buffer_error &) {
                        rejected = true;
                    }

                    expect(rejected);
                };

                "writes lazy ranges of text chunks"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{8}};
                    auto numbers = std::views::iota(1, 4);
                    auto chunks = numbers
                        | std::views::transform([](int n) {
                            return std::to_string(n);
                        });

                    co_await nxtrt::write(writer, chunks);
                    co_await writer.flush();

                    expect(writer.text == "123");
                };

                "writes ranges of byte spans"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{4}};
                    auto chunks = std::array{
                        nxtrt::as_bytes("ab"sv),
                        nxtrt::as_bytes("cd"sv),
                        nxtrt::as_bytes("ef"sv),
                    };

                    co_await nxtrt::write(writer, chunks);
                    co_await writer.flush();

                    expect(writer.text == "abcdef");
                };

                "writes and flushes byte spans"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{64, std::size_t{8}};
                    auto chunks = std::array{
                        nxtrt::as_bytes("ab"sv),
                        nxtrt::as_bytes("cd"sv),
                        nxtrt::as_bytes("ef"sv),
                    };

                    co_await nxtrt::write_all(writer, chunks);

                    expect(writer.text == "abcdef");
                    expect(writer.buffered_size() == std::size_t{0});
                };

                "free write_all borrows lvalue sinks"_test = []() -> nxtrt::task<void> {
                    auto sink = chunking_string_sink{64};
                    auto chunks =
                        std::vector<std::string>{"ab", "cd", "e"};

                    co_await nxtrt::write_all(sink, chunks);

                    expect(sink.text == "abcde");
                };

                "free write_all uses explicitly owned sinks"_test = []() -> nxtrt::task<void> {
                    auto text = std::make_shared<std::string>();
                    auto sink = shared_string_sink{text};
                    auto chunks =
                        std::vector<std::string>{"ab", "cd", "e"};

                    co_await nxtrt::write_all(sink, chunks);

                    expect(*text == "abcde");
                };
            };

            "with borrowed sink and owned storage"_group = [] {
                "buffers bytes until flush"_test = []() -> nxtrt::task<void> {
                    auto text = std::make_shared<std::string>();
                    auto writer = shared_string_sink{
                        text,
                        std::size_t{64},
                        std::size_t{4},
                    };

                    co_await nxtrt::write(writer, std::string{"abc"});
                    expect(text->empty());
                    co_await nxtrt::write(writer, std::string{"de"});
                    expect(*text == "abcd");
                    co_await writer.flush();

                    expect(*text == "abcde");
                };
            };

            "with zero storage"_group = [] {
                "owned zero-size buffers write directly"_test = []() -> nxtrt::task<void> {
                    auto writer = chunking_string_sink{2, std::size_t{0}};

                    co_await nxtrt::write(writer, "abcde"sv);
                    expect(writer.text == "abcde");
                    expect(writer.buffered_size() == std::size_t{0});
                    co_await writer.flush();

                    expect(writer.text == "abcde");
                };

                "borrowed empty buffers write directly"_test = []() -> nxtrt::task<void> {
                    auto writer =
                        chunking_string_sink{64, std::span<std::byte>{}};

                    co_await nxtrt::write(writer, "ab"sv);
                    expect(writer.text == "ab");
                    expect(writer.buffered_size() == std::size_t{0});
                    co_await writer.flush();

                    expect(writer.text == "ab");
                };
            };
        };
    };
}

} // namespace nxt::test
