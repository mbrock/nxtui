#include "runtime-test.hpp"

namespace nxt::test {

void declare_runtime_io_tests()
{
    "feeds and sinks"_group = [] {
        "peek fills the source buffer without consuming"_test = []() -> nxtrt::task<void> {
            auto storage = nxtrt::static_value_storage<int, 1>{};
            auto source = int_feed{
                std::vector<int>{1, 2},
                storage,
            };

            co_await check_feed_peek(source);
        };

        "peek borrows requested values as chunks"_test = []() -> nxtrt::task<void> {
            auto source = int_feed{std::vector<int>{1, 2, 3}, 2};

            co_await check_feed_chunk_peek(source);
        };

        "feed buffers expose wrapped chunks"_test = []() -> nxtrt::task<void> {
            auto source =
                int_feed{std::vector<int>{1, 2, 3, 4, 5}, 3};

            co_await check_feed_ring_peek(source);
        };

        "byte feeds have reader-shaped ring lookahead"_test = []() -> nxtrt::task<void> {
            auto text = "abcdef"sv;
            auto storage = nxtrt::static_value_storage<std::byte, 4>{};
            auto source = nxtrt::value_range_source{
                nxtrt::as_bytes(text),
                storage,
            };

            co_await check_byte_feed_ring_shape(source);
        };

        "bytefeeds are feeds"_test = []() -> nxtrt::task<void> {
            auto chunks = std::array{"abcdef"sv};
            auto storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{storage});

            co_await check_byte_feed_ring_shape(reader);
        };

        "peek returns null at eof"_test = []() -> nxtrt::task<void> {
            auto source = int_feed{std::vector<int>{}, 1};

            auto event = co_await peek_int_value(source);

            expect(event == nullptr);
        };

        "peek_one and take_one throw at eof"_test = [] {
            auto deck = nxtrt::deck{};
            auto source = int_feed{std::vector<int>{7}, 1};

            deck.sync_wait([&] {
                return check_feed_one_methods(source);
            });

            auto rejected = false;
            try {
                (void)deck.sync_wait([&] {
                    return take_one_int_value(source);
                });
            } catch (const nxtrt::value_end_of_stream &) {
                rejected = true;
            }

            expect(rejected);
        };

        "sink buffers values until flush"_test = []() -> nxtrt::task<void> {
            auto storage = nxtrt::static_value_storage<int, 2>{};
            auto sink = collecting_int_sink{64, storage};

            co_await check_sink_buffers_until_flush(sink);

            expect(sink.collected == std::vector<int>{1, 2});
        };

        "sink buffers expose wrapped chunks"_test = []() -> nxtrt::task<void> {
            auto sink = collecting_int_sink{2, std::size_t{3}};

            co_await check_sink_ring_buffer(sink);

            expect(sink.collected == std::vector<int>{1, 2, 3, 4, 5});
        };

        "sinks write value spans and splats"_test = []() -> nxtrt::task<void> {
            auto sink = collecting_int_sink{64, std::size_t{8}};
            auto values = std::array{1, 2, 3};
            auto pattern = std::array{8, 9};

            co_await sink.write(std::span<const int>{values});
            co_await sink.write_splat(std::span<const int>{pattern}, 2);
            co_await sink.flush();

            expect(sink.collected == std::vector<int>{1, 2, 3, 8, 9, 8, 9});
        };

        "zero-storage sinks drain splatted value chunks"_test = []() -> nxtrt::task<void> {
            auto sink = collecting_int_sink{64, std::size_t{0}};
            auto pattern = std::array{4, 5};

            co_await sink.write_splat(std::span<const int>{pattern}, 3);

            expect(sink.collected == std::vector<int>{4, 5, 4, 5, 4, 5});
        };

        "stream_all moves feed values into sinks"_test = []() -> nxtrt::task<void> {
            auto source = int_feed{std::vector<int>{1, 2, 3}, 1};
            auto sink = collecting_int_sink{64, std::size_t{2}};

            auto streamed = co_await nxtrt::stream_all(source, sink);

            expect(streamed == std::size_t{3});
            expect(sink.collected == std::vector<int>{1, 2, 3});
        };

        "zero-storage feeds stream directly into sinks"_test = []() -> nxtrt::task<void> {
            auto source = int_feed{
                std::vector<int>{5, 6, 7},
                std::size_t{0},
            };
            auto sink = collecting_int_sink{64, std::size_t{3}};

            auto streamed = co_await nxtrt::stream_all(source, sink);

            expect(streamed == std::size_t{3});
            expect(sink.collected == std::vector<int>{5, 6, 7});
        };

        "container sinks append without internal storage"_test = []() -> nxtrt::task<void> {
            auto values = std::vector<int>{};
            auto sink = nxtrt::container_sink{values};

            co_await write_int_values(sink, 1, 2);
            expect(sink.buffered_size() == std::size_t{0});

            expect(values == std::vector<int>{1, 2});
        };

        "iterator sinks write through output iterators"_test = []() -> nxtrt::task<void> {
            auto values = std::vector<int>{};
            auto sink = nxtrt::iterator_sink<
                int,
                decltype(std::back_inserter(values))>{
                std::back_inserter(values),
            };

            co_await write_int_values(sink, 3, 4);

            expect(values == std::vector<int>{3, 4});
        };

        "range feeds stream lazy views"_test = []() -> nxtrt::task<void> {
            auto values = std::vector<int>{};
            auto source = nxtrt::value_range_source{
                std::views::iota(1, 4)
                    | std::views::transform([](int n) {
                        return n * 10;
                    }),
                std::size_t{1},
            };
            auto sink = nxtrt::container_sink{values};

            auto streamed = co_await nxtrt::stream_all(source, sink);

            expect(streamed == std::size_t{3});
            expect(values == std::vector<int>{10, 20, 30});
        };

        "taskfeeds fill typed value storage from task callables"_test = []() -> nxtrt::task<void> {
            struct read_ints
            {
                nxtrt::task<nxtrt::fare_t>
                operator()(nxtrt::junk<int> dst)
                {
                    auto n = std::min(dst.size(), values.size() - offset);
                    for (auto i = std::size_t{0}; i < n; ++i)
                        std::construct_at(dst.data() + i, values[offset + i]);
                    offset += n;
                    if (n == 0)
                        co_return nxtrt::eof;
                    co_return n;
                }

                std::array<int, 4> values{1, 2, 3, 4};
                std::size_t offset = 0;
            };

            auto storage = std::array<int, 3>{};
            auto source =
                nxtrt::taskfeed{read_ints{}, std::span{storage}};
            auto out = std::vector<int>{};
            auto sink = nxtrt::container_sink{out};

            auto peeked = co_await source.peek(2);
            expect(peeked.size() == std::size_t{2});
            auto streamed = co_await nxtrt::stream_all(source, sink);
            expect(streamed == std::size_t{4});

            expect(out == std::vector<int>{1, 2, 3, 4});
        };

        "feeds peek and take structs over value atoms"_test = []() -> nxtrt::task<void> {
            struct triple
            {
                int a = 0;
                int b = 0;
                int c = 0;
            };

            auto source =
                int_feed{std::vector<int>{1, 2, 3, 4}, 3};

            auto peeked = co_await source.peek_struct<triple>();
            expect(peeked.a == 1_i);
            expect(peeked.b == 2_i);
            expect(peeked.c == 3_i);
            expect(source.buffered_size() == std::size_t{3});

            auto taken = co_await source.take_struct<triple>();
            expect(taken.has_value());
            expect(taken->a == 1_i);
            expect(taken->b == 2_i);
            expect(taken->c == 3_i);
            expect(source.buffered_size() == std::size_t{0});

            auto rest = co_await source.take_one();
            expect(rest == 4_i);
        };

        "parser feeds parse values from arbitrary feeds"_test = []() -> nxtrt::task<void> {
            auto input = int_feed{std::vector<int>{1, 2, 3, 4}, 2};
            auto source =
                nxtrt::function_parser_feed<int, int>{
                    input,
                    parse_summed_pair,
                };
            auto values = std::vector<int>{};
            auto sink = nxtrt::container_sink{values};

            auto streamed = co_await nxtrt::stream_all(source, sink);

            expect(streamed == std::size_t{2});
            expect(values == std::vector<int>{3, 7});
            expect(input.buffered_size() == std::size_t{0});
        };

        "byte parsers stream parsed values from bytefeeds"_test = []() -> nxtrt::task<void> {
            auto chunks = std::array{"123"sv};
            auto storage = std::array<std::byte, 4>{};
            auto reader = text_source(chunks, std::span{storage});
            auto source =
                nxtrt::byte_parser<int>{reader, parse_digit_value};
            auto values = std::vector<int>{};
            auto sink = nxtrt::container_sink{values};

            auto streamed = co_await nxtrt::stream_all(source, sink);

            expect(streamed == std::size_t{3});
            expect(values == std::vector<int>{1, 2, 3});
        };

        "range feed lookahead uses source storage"_test = []() -> nxtrt::task<void> {
            auto storage = nxtrt::static_value_storage<int, 1>{};
            auto source = nxtrt::value_range_source{
                std::views::iota(5, 7),
                storage,
            };

            co_await check_range_source_lookahead(source);
        };

        "discard_all consumes expected values"_test = []() -> nxtrt::task<void> {
            auto source = int_feed{std::vector<int>{1, 2, 3}, 1};

            co_await discard_expected_prefix(source);
            auto rest = co_await take_int_value(source);
            expect(rest && *rest == 3_i);
        };

        "discard_all leaves mismatched values buffered"_test = [] {
            auto deck = nxtrt::deck{};
            auto source = int_feed{std::vector<int>{1, 2, 3}, 1};
            auto rejected = false;

            try {
                deck.sync_wait([&] {
                    return discard_mismatched_prefix(source);
                });
            } catch (const nxtrt::unexpected_value &) {
                rejected = true;
            }

            expect(rejected);
            auto next = deck.sync_wait([&] {
                return take_int_value(source);
            });
            expect(next && *next == 2_i);
        };

        "discard_all throws at eof"_test = [] {
            auto deck = nxtrt::deck{};
            auto source = int_feed{std::vector<int>{1}, 1};
            auto rejected = false;

            try {
                deck.sync_wait([&] {
                    return discard_past_eof(source);
                });
            } catch (const nxtrt::value_end_of_stream &) {
                rejected = true;
            }

            expect(rejected);
        };
    };

    "wires"_group = [] {
        "buffer values until consumed"_test = [] {
            auto deck = nxtrt::deck{};
            auto storage = nxtrt::rack<int>{2};
            auto events = nxtrt::wire<int>{storage};

            expect(events.try_send(1));
            expect(events.try_send(2));

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
            auto rt = nxtrt::runtime{};
            auto storage = nxtrt::rack<int>{64};
            auto events = nxtrt::wire<int>{storage};
            auto seen = std::vector<int>{};

            rt.run([&]() -> nxtrt::task<void> {
                (void)co_await nxtrt::when_all(std::tuple{
                    [&] { return record_next_wire_value(events, seen); },
                    [&]() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                        expect(seen.empty());
                        expect(co_await events.send(7));
                    },
                });
            });

            expect(seen == std::vector<int>{7});
        };

        "close rejects publishers and drains consumers"_test = []() -> nxtrt::task<void> {
            auto storage = nxtrt::rack<int>{64};
            auto events = nxtrt::wire<int>{storage};

            expect(events.try_send(1));
            events.close();

            std::optional<int> first = co_await events.next();
            std::optional<int> second = co_await events.next();

            expect(first && *first == 1_i);
            expect(!second);
            expect(!events.try_send(2));
        };

        "close wakes pending consumers"_test = [] {
            auto rt = nxtrt::runtime{};
            auto storage = nxtrt::rack<int>{64};
            auto events = nxtrt::wire<int>{storage};
            auto finished = false;

            rt.run([&]() -> nxtrt::task<void> {
                (void)co_await nxtrt::when_all(std::tuple{
                    [&] { return record_closed_wire(events, finished); },
                    [&]() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                        events.close();
                    },
                });
            });

            expect(events.closed());
            expect(finished);
            expect(!events.try_send(1));
        };

        "try_next drains buffered values without awaiting"_test = [] {
            auto storage = nxtrt::rack<int>{64};
            auto events = nxtrt::wire<int>{storage};

            expect(events.try_send(3));
            auto value = events.try_next();
            expect(value && *value == 3_i);
            expect(!events.try_next());
        };

        "tx and rx sides expose directional operations"_test = [] {
            auto storage = nxtrt::rack<int>{1};
            auto events = nxtrt::wire<int>{storage};
            auto & tx = events.tx();
            auto & rx = events.rx();

            expect(tx.capacity() == std::size_t{1});
            expect(rx.capacity() == std::size_t{1});
            expect(tx.try_send(5));
            expect(tx.full());

            auto value = rx.try_next();
            expect(value && *value == 5_i);
            expect(rx.empty());

            tx.close();
            expect(tx.closed());
            expect(rx.closed());
            expect(!tx.try_send(6));
        };

        "wire receives through feed operations"_test = [] {
            auto deck = nxtrt::deck{};
            auto storage = nxtrt::rack<int>{2};
            auto events = nxtrt::wire<int>{storage};

            expect(events.try_send(8));
            expect(events.try_send(9));

            auto values = deck.sync_wait([&]() -> nxtrt::task<
                std::vector<int>> {
                auto out = std::vector<int>{};
                auto first = co_await events.rx().take();
                auto second = co_await events.rx().take();
                if (first)
                    out.push_back(*first);
                if (second)
                    out.push_back(*second);
                co_return out;
            });

            expect(values == std::vector<int>{8, 9});
        };

        "wire bundles structure bind to rx and tx endpoints"_test = [] {
            auto deck = nxtrt::deck{};
            auto storage = nxtrt::rack<int>{1};
            auto [rx, tx] = nxtrt::wire<int>{storage};

            expect(tx.try_send(21));

            auto value = deck.sync_wait([&]() -> nxtrt::task<
                std::optional<int>> {
                co_return co_await rx.take();
            });

            expect(value && *value == 21_i);
        };

        "tx side writes through sink operations"_test = [] {
            auto rt = nxtrt::runtime{};
            auto storage = nxtrt::rack<int>{1};
            auto events = nxtrt::wire<int>{storage};
            auto seen = std::vector<int>{};

            rt.run([&]() -> nxtrt::task<void> {
                auto & tx = events.tx();
                co_await tx.write(12);
                (void)co_await nxtrt::when_all(std::tuple{
                    [&] { return record_next_wire_value(events, seen); },
                    [&]() -> nxtrt::task<void> {
                        co_await tx.write(13);
                        co_await record_next_wire_value(events, seen);
                    },
                });
            });

            expect(seen == std::vector<int>{12, 13});
        };

        "bounded queues reject immediate sends when full"_test = [] {
            auto storage = nxtrt::rack<int>{1};
            auto events = nxtrt::wire<int>{storage};

            expect(events.try_send(1));
            expect(!events.try_send(2));
        };

        "zero-buffer wires rendezvous sender and receiver"_test = [] {
            auto rt = nxtrt::runtime{};
            auto storage = nxtrt::rack<int>{0};
            auto events = nxtrt::wire<int>{storage};
            auto seen = std::vector<int>{};
            auto sent = false;

            expect(events.capacity() == std::size_t{0});
            expect(!events.try_send(1));

            rt.run([&]() -> nxtrt::task<void> {
                (void)co_await nxtrt::when_all(std::tuple{
                    [&] { return send_wire_value(events, 42, sent); },
                    [&]() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                        expect(!sent);
                        co_await record_next_wire_value(events, seen);
                    },
                });
            });

            expect(sent);
            expect(seen == std::vector<int>{42});
        };

        "flush waits until accepted values are consumed"_test = [] {
            auto rt = nxtrt::runtime{};
            auto storage = nxtrt::rack<int>{2};
            auto events = nxtrt::wire<int>{storage};
            auto seen = std::vector<int>{};
            auto flushed = false;

            expect(events.try_send(3));

            rt.run([&]() -> nxtrt::task<void> {
                (void)co_await nxtrt::when_all(std::tuple{
                    [&] { return flush_wire(events, flushed); },
                    [&]() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                        expect(!flushed);
                        co_await record_next_wire_value(events, seen);
                    },
                });
            });

            expect(flushed);
            expect(seen == std::vector<int>{3});
        };

        "send then flush acts like an unbuffered write"_test = [] {
            auto rt = nxtrt::runtime{};
            auto storage = nxtrt::rack<int>{1};
            auto events = nxtrt::wire<int>{storage};
            auto seen = std::vector<int>{};
            auto flushed = false;

            rt.run([&]() -> nxtrt::task<void> {
                expect(co_await events.send(4));
                (void)co_await nxtrt::when_all(std::tuple{
                    [&] { return flush_wire(events, flushed); },
                    [&]() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                        expect(!flushed);
                        co_await record_next_wire_value(events, seen);
                    },
                });
            });

            expect(flushed);
            expect(seen == std::vector<int>{4});
        };
    };

    "bells"_group = [] {
        "ring wakes waiting tasks"_test = [] {
            auto rt = nxtrt::runtime{};
            auto ready = nxtrt::bell{};
            auto values = std::vector<int>{};

            rt.run([&]() -> nxtrt::task<void> {
                (void)co_await nxtrt::when_all(std::tuple{
                    [&]() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                        expect(values.empty());
                        ready.ring();
                    },
                    [&] { return record_after_bell(ready, values, 1); },
                    [&] { return record_after_bell(ready, values, 2); },
                });
            });

            expect(values == std::vector<int>{1, 2});
        };

        "reset makes future awaits suspend again"_test = [] {
            auto rt = nxtrt::runtime{};
            auto ready = nxtrt::bell{};
            auto values = std::vector<int>{};

            ready.ring();
            rt.run([&]() -> nxtrt::task<void> {
                co_await ready;
                values.push_back(1);
            });

            ready.reset();

            rt.run([&]() -> nxtrt::task<void> {
                (void)co_await nxtrt::when_all(std::tuple{
                    [&] { return record_after_bell(ready, values, 2); },
                    [&]() -> nxtrt::task<void> {
                        co_await nxtrt::yield();
                        expect(values == std::vector<int>{1});
                        ready.ring();
                    },
                });
            });

            expect(values == std::vector<int>{1, 2});
        };
    };

    "HTTP requests"_group = [] {
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

    "HTTP bodies"_group = [] {
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

                    auto body = nxtrt::http::response_body_decoding_reader(
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

                        auto body = nxtrt::http::response_body_decoding_reader(
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

        "gzip content-encoding is inflated after transfer decoding"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto compressed = gzip_text(
                    "event: response.output_text.delta\n"
                    "data: {\"delta\":\"hi\"}\n"
                    "\n");
                auto wire =
                    "HTTP/1.1 200 OK\r\nContent-Length: "s
                    + std::to_string(compressed.size())
                    + "\r\nContent-Encoding: gzip\r\n\r\n"
                    + compressed
                    + "HTTP/1.1 204 No Content\r\n"
                      "Content-Length: 0\r\n\r\n";
                auto chunks = std::array{std::string_view{wire}};
                auto storage = std::array<std::byte, 256>{};
                auto reader = text_source(chunks, std::span{storage});

                deck.sync_wait([&]() -> nxtrt::task<void> {
                    auto head =
                        co_await nxtrt::http::read_response_head(
                            reader);
                    expect(head.status == 200_i);

                    auto body = nxtrt::http::response_body_decoding_reader(
                        reader, head);
                    auto event =
                        co_await nxtrt::http::parse_sse_event(body);
                    expect(event.has_value());
                    expect(event->type == "response.output_text.delta");
                    expect(event->data == "{\"delta\":\"hi\"}");

                    auto second =
                        co_await nxtrt::http::read_response_head(
                            reader);
                    expect(second.status == 204_i);
                });
            };

        "deflate content-encoding is inflated after transfer decoding"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto compressed = zlib_text("hello deflate");
                auto wire =
                    "HTTP/1.1 200 OK\r\nContent-Length: "s
                    + std::to_string(compressed.size())
                    + "\r\nContent-Encoding: deflate\r\n\r\n"
                    + compressed;
                auto chunks = std::array{std::string_view{wire}};
                auto storage = std::array<std::byte, 128>{};
                auto reader = text_source(chunks, std::span{storage});

                auto result =
                    deck.sync_wait([&]() -> nxtrt::task<std::string> {
                        auto head =
                            co_await nxtrt::http::read_response_head(
                                reader);
                        auto body =
                            nxtrt::http::response_body_decoding_reader(
                                reader, head);
                        auto text = std::string{};
                        while (auto chunk = co_await body.next())
                            text += nxtrt::as_string_view(*chunk);
                        co_return text;
                    });

                expect(result == "hello deflate");
            };

        #if defined(NXTRT_HAVE_ZSTD)
        "zstd content-encoding is decompressed after transfer decoding"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto compressed = zstd_text("hello zstd");
                auto wire =
                    "HTTP/1.1 200 OK\r\nContent-Length: "s
                    + std::to_string(compressed.size())
                    + "\r\nContent-Encoding: zstd\r\n\r\n"
                    + compressed;
                auto chunks = std::array{std::string_view{wire}};
                auto storage = std::array<std::byte, 128>{};
                auto reader = text_source(chunks, std::span{storage});

                auto result =
                    deck.sync_wait([&]() -> nxtrt::task<std::string> {
                        auto head =
                            co_await nxtrt::http::read_response_head(
                                reader);
                        auto body =
                            nxtrt::http::response_body_decoding_reader(
                                reader, head);
                        auto text = std::string{};
                        while (auto chunk = co_await body.next())
                            text += nxtrt::as_string_view(*chunk);
                        co_return text;
                    });

                expect(result == "hello zstd");
            };
        #endif

        #if defined(NXTRT_HAVE_BROTLI)
        "brotli content-encoding is decompressed after transfer decoding"_test =
            [] {
                auto deck = nxtrt::deck{};
                auto compressed = brotli_hello_text();
                auto wire =
                    "HTTP/1.1 200 OK\r\nContent-Length: "s
                    + std::to_string(compressed.size())
                    + "\r\nContent-Encoding: br\r\n\r\n"
                    + compressed;
                auto chunks = std::array{std::string_view{wire}};
                auto storage = std::array<std::byte, 128>{};
                auto reader = text_source(chunks, std::span{storage});

                auto result =
                    deck.sync_wait([&]() -> nxtrt::task<std::string> {
                        auto head =
                            co_await nxtrt::http::read_response_head(
                                reader);
                        auto body =
                            nxtrt::http::response_body_decoding_reader(
                                reader, head);
                        auto text = std::string{};
                        while (auto chunk = co_await body.next())
                            text += nxtrt::as_string_view(*chunk);
                        co_return text;
                    });

                expect(result == "hello brotli");
            };
        #endif

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

            auto events =
                deck.sync_wait([&] {
                    return read_sse_events_from_response(reader);
                });

            expect(events.size() == std::size_t{1});
            expect(events[0].type == "response.output_text.delta");
            expect(events[0].data == "{\"delta\":\"hi\"}");
        };
    };
}

} // namespace nxt::test
