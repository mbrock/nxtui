#pragma once

// Shared fixtures for the runtime suite, whose groups are split across
// runtime-*-test.cpp so no single translation unit dominates the build.

#include <nxt/sparkline.hpp>
#include <nxtui/tui_text.hpp>
#include <nxtrt/app.hpp>
#include <nxtrt/buffers.hpp>
#include <nxtrt/bell.hpp>
#include <nxtrt/compression.hpp>
#include <nxtrt/farm.hpp>
#include <nxtrt/game.hpp>
#include <nxtrt/http.hpp>
#include <nxtrt/wand/kqueue.hpp>
#include <nxtrt/sampling.hpp>
#include <nxtrt/task.hpp>
#include <nxtrt/terminal_app.hpp>
#include <nxtrt/value-buffers.hpp>
#include <nxtrt/wire.hpp>
#include <nxtai/tool_batch.hpp>

#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <zlib.h>

namespace nxt::test {

using namespace nxtui;

using namespace boost::ut;
using namespace std::literals;

template<std::ranges::viewable_range Range>
auto text_source(Range && chunks, std::span<std::byte> storage)
{
    return nxtrt::byte_span_feed{
        std::forward<Range>(chunks),
        storage,
    };
}

inline std::string deflated_text(std::string_view text, int window_bits)
{
    auto stream = z_stream{};
    auto rc = ::deflateInit2(
        &stream,
        Z_BEST_SPEED,
        Z_DEFLATED,
        window_bits,
        8,
        Z_DEFAULT_STRATEGY);
    if (rc != Z_OK)
        throw std::runtime_error{"deflateInit2 failed"};

    auto end = false;
    auto out = std::string{};
    auto buffer = std::array<char, 128>{};
    stream.next_in = reinterpret_cast<Bytef *>(
        const_cast<char *>(text.data()));
    stream.avail_in = static_cast<uInt>(text.size());

    while (!end) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());

        rc = ::deflate(&stream, Z_FINISH);
        if (rc == Z_STREAM_END) {
            end = true;
        } else if (rc != Z_OK) {
            ::deflateEnd(&stream);
            throw std::runtime_error{"deflate failed"};
        }

        out.append(buffer.data(), buffer.size() - stream.avail_out);
    }

    ::deflateEnd(&stream);
    return out;
}

inline std::string gzip_text(std::string_view text)
{
    return deflated_text(text, MAX_WBITS + 16);
}

inline std::string zlib_text(std::string_view text)
{
    return deflated_text(text, MAX_WBITS);
}

#if defined(NXTRT_HAVE_ZSTD)
inline std::string zstd_text(std::string_view text)
{
    auto out = std::string(ZSTD_compressBound(text.size()), '\0');
    auto n = ZSTD_compress(
        out.data(),
        out.size(),
        text.data(),
        text.size(),
        1);
    if (ZSTD_isError(n))
        throw std::runtime_error{ZSTD_getErrorName(n)};
    out.resize(n);
    return out;
}
#endif

#if defined(NXTRT_HAVE_BROTLI)
inline std::string brotli_hello_text()
{
    const auto bytes = std::array<char, 16>{
        '\x8b',
        '\x05',
        '\x80',
        'h',
        'e',
        'l',
        'l',
        'o',
        ' ',
        'b',
        'r',
        'o',
        't',
        'l',
        'i',
        '\x03',
    };
    return std::string{bytes.data(), bytes.size()};
}
#endif

struct ambient_int_key
{
    using value_type = int;
    static constexpr auto name = "ambient-int";
};

struct manual_wand final : nxtrt::wand
{
    void
    suspend(nxtrt::coin_t token, nxtrt::need task) override
    {
        parked.push_back(
            parked_entry{
                .token = token,
                .task = task,
            });
    }

    void cancel(nxtrt::coin_t token) override
    {
        cancelled.push_back(token);
    }

    void wave(nxtrt::deck &) override
    {
        ++waves;
    }

    void fulfill(nxtrt::deck & deck, nxtrt::coin_t token)
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
    nxtrt::coin_t prep(
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
            std::static_pointer_cast<nxtrt::urge_state<void>>(
                packet.state));
        return wish->token;
    }

public:
    struct parked_entry
    {
        nxtrt::coin_t token = 0;
        nxtrt::need task;
    };

    std::vector<nxtrt::coin_t> prepared;
    std::vector<nxtrt::coin_t> cancelled;
    std::vector<parked_entry> parked;
    std::vector<std::shared_ptr<nxtrt::urge_state<void>>> states;
    int waves = 0;
};

struct empty_then_string_source final : nxtrt::bytefeed
{
    explicit empty_then_string_source(std::size_t buffer_size = 8)
        : nxtrt::bytefeed(buffer_size)
    {}

private:
    nxtrt::hope<nxtrt::fare_t> stream_more(
        nxtrt::bytesink & writer,
        std::size_t limit) override
    {
        if (limit == 0)
            return nxtrt::hope<nxtrt::fare_t>::ready(
                0);
        if (!returned_empty) {
            returned_empty = true;
            return nxtrt::hope<nxtrt::fare_t>::ready(
                0);
        }

        if (offset == text.size()) {
            return nxtrt::hope<nxtrt::fare_t>::ready(
                nxtrt::eof);
        }

        auto rest = std::string_view{text}.substr(offset);
        auto n = std::min(limit, rest.size());
        auto dst = writer.unused_capacity();
        if (!dst.empty())
            n = std::min(n, dst.size());
        auto write = nxtrt::write(writer, rest.substr(0, n));
        if (!write.is_ready())
            return stream_write_slow(std::move(write), n);
        offset += n;
        if (n == 0 && offset == text.size())
            return nxtrt::hope<nxtrt::fare_t>::ready(nxtrt::eof);
        return nxtrt::hope<nxtrt::fare_t>::ready(
            n);
    }

    nxtrt::task<nxtrt::fare_t> stream_write_slow(
        nxtrt::hope<void> write,
        std::size_t n)
    {
        co_await std::move(write);
        offset += n;
        if (n == 0 && offset == text.size())
            co_return nxtrt::eof;
        co_return n;
    }

public:
    std::string_view text = "abc";
    std::size_t offset = 0;
    bool returned_empty = false;
};

struct chunking_string_sink final : nxtrt::bytesink
{
    explicit chunking_string_sink(
        std::size_t limit,
        std::size_t buffer_size = 64)
        : nxtrt::bytesink(buffer_size)
        , limit(limit)
    {}

    chunking_string_sink(
        std::size_t limit,
        std::span<std::byte> buffer)
        : nxtrt::bytesink(buffer)
        , limit(limit)
    {}

private:
    nxtrt::hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override
    {
        if (chunks.empty())
            return nxtrt::hope<std::size_t>::ready(0);
        auto remaining = limit;
        auto n = std::size_t{0};
        auto append = [&](std::span<const std::byte> chunk) {
            auto take = std::min(remaining, chunk.size());
            text += nxtrt::as_string_view(chunk.first(take));
            n += take;
            remaining -= take;
        };

        auto spans = chunks.chunks();
        for (auto chunk : spans.first(spans.size() - 1)) {
            append(chunk);
            if (remaining == 0)
                return nxtrt::hope<std::size_t>::ready(n);
        }
        for (auto i = std::size_t{0}; i < splat && remaining != 0; ++i)
            append(spans.back());
        return nxtrt::hope<std::size_t>::ready(n);
    }

public:
    std::string text;
    std::size_t limit = 1;
};

struct shared_string_sink final : nxtrt::bytesink
{
    explicit shared_string_sink(
        std::shared_ptr<std::string> text,
        std::size_t limit = 64,
        std::size_t buffer_size = 64)
        : nxtrt::bytesink(buffer_size)
        , text(std::move(text))
        , limit(limit)
    {}

    shared_string_sink(
        std::shared_ptr<std::string> text,
        std::size_t limit,
        std::span<std::byte> buffer)
        : nxtrt::bytesink(buffer)
        , text(std::move(text))
        , limit(limit)
    {}

private:
    nxtrt::hope<std::size_t>
    drain_more(
        value_chunk_view chunks,
        std::size_t splat) override
    {
        if (chunks.empty())
            return nxtrt::hope<std::size_t>::ready(0);
        auto remaining = limit;
        auto n = std::size_t{0};
        auto append = [&](std::span<const std::byte> chunk) {
            auto take = std::min(remaining, chunk.size());
            *text += nxtrt::as_string_view(chunk.first(take));
            n += take;
            remaining -= take;
        };

        auto spans = chunks.chunks();
        for (auto chunk : spans.first(spans.size() - 1)) {
            append(chunk);
            if (remaining == 0)
                return nxtrt::hope<std::size_t>::ready(n);
        }
        for (auto i = std::size_t{0}; i < splat && remaining != 0; ++i)
            append(spans.back());
        return nxtrt::hope<std::size_t>::ready(n);
    }

public:
    std::shared_ptr<std::string> text;
    std::size_t limit = 1;
};

struct int_feed final : nxtrt::feed<int>
{
    int_feed(
        std::vector<int> values,
        nxtrt::value_storage_ref<int> buffer)
        : nxtrt::feed<int>(buffer)
        , values(std::move(values))
    {}

    int_feed(std::vector<int> values, std::size_t buffer_size = 1)
        : nxtrt::feed<int>(buffer_size)
        , values(std::move(values))
    {}

private:
    nxtrt::hope<nxtrt::fare_t> stream_more(
        nxtrt::sink<int> & sink,
        std::size_t limit) override
    {
        if (limit == 0)
            return nxtrt::hope<nxtrt::fare_t>::ready(
                0);
        if (offset == values.size())
            return nxtrt::hope<nxtrt::fare_t>::ready(
                nxtrt::eof);

        auto write = sink.write(values[offset]);
        if (write.is_ready()) {
            ++offset;
            ++reads;
            return nxtrt::hope<nxtrt::fare_t>::ready(
                1);
        }

        return stream_write_slow(std::move(write));
    }

    nxtrt::task<nxtrt::fare_t> stream_write_slow(
        nxtrt::hope<void> write)
    {
        co_await std::move(write);
        ++offset;
        ++reads;
        co_return 1;
    }

public:
    std::vector<int> values;
    std::size_t offset = 0;
    std::size_t reads = 0;
};

struct collecting_int_sink final : nxtrt::sink<int>
{
    explicit collecting_int_sink(
        std::size_t limit,
        std::size_t buffer_size = 64)
        : nxtrt::sink<int>(buffer_size)
        , limit(limit)
    {}

    collecting_int_sink(
        std::size_t limit,
        nxtrt::value_storage_ref<int> buffer)
        : nxtrt::sink<int>(buffer)
        , limit(limit)
    {}

private:
    nxtrt::hope<std::size_t>
    drain_more(
        nxtrt::sink<int>::value_chunk_view values,
        std::size_t splat) override
    {
        auto n = std::size_t{0};
        auto append = [&](auto chunk) {
            for (auto & value : chunk) {
                if (n == limit)
                    return false;
                collected.push_back(value);
                ++n;
            }
            return true;
        };

        if (values.empty())
            return nxtrt::hope<std::size_t>::ready(0);

        auto chunks = values.chunks();
        for (auto chunk : chunks.first(chunks.size() - 1)) {
            if (!append(chunk))
                return nxtrt::hope<std::size_t>::ready(n);
        }
        for (auto i = std::size_t{0}; i < splat; ++i) {
            if (!append(chunks.back()))
                return nxtrt::hope<std::size_t>::ready(n);
        }
        return nxtrt::hope<std::size_t>::ready(n);
    }

public:
    std::vector<int> collected;
    std::size_t limit = 1;
};

inline nxtrt::task<void> check_feed_peek(int_feed & source)
{
    auto first = co_await source.peek();
    expect(first != nullptr);
    expect(*first == 1_i);
    expect(source.reads == std::size_t{1});

    auto again = co_await source.peek();
    expect(again != nullptr);
    expect(*again == 1_i);
    expect(source.reads == std::size_t{1});

    auto taken = co_await source.take();
    expect(taken && *taken == 1_i);
    expect(source.buffered_size() == std::size_t{0});

    auto next = co_await source.take();
    expect(next && *next == 2_i);
    expect(source.reads == std::size_t{2});
}

inline nxtrt::task<void> check_feed_chunk_peek(int_feed & source)
{
    auto values = co_await source.peek(2);
    expect(values.size() == std::size_t{2});
    expect(values.chunk_count() == std::size_t{1});

    auto seen = std::vector<int>{};
    for (auto chunk : values) {
        for (auto value : chunk)
            seen.push_back(value);
    }
    expect(seen == std::vector<int>{1, 2});
    expect(source.buffered_size() == std::size_t{2});
    expect(source.reads == std::size_t{2});

    auto discarded = co_await source.discard(2);
    expect(value_count(discarded) == std::size_t{2});

    auto next = co_await source.take();
    expect(next && *next == 3_i);
}

inline nxtrt::task<void> check_feed_ring_peek(int_feed & source)
{
    auto initial = co_await source.peek(3);
    expect(initial.size() == std::size_t{3});
    auto discarded = co_await source.discard(2);
    expect(value_count(discarded) == std::size_t{2});

    auto wrapped = co_await source.peek(3);
    expect(wrapped.size() == std::size_t{3});
    expect(wrapped.chunk_count() == std::size_t{2});

    auto seen = std::vector<int>{};
    for (auto chunk : wrapped) {
        for (auto value : chunk)
            seen.push_back(value);
    }
    expect(seen == std::vector<int>{3, 4, 5});
}

inline std::string byte_value_chunks_text(
    nxtrt::value_chunks<const std::byte> chunks)
{
    auto out = std::string{};
    for (auto chunk : chunks)
        out += nxtrt::as_string_view(chunk);
    return out;
}

inline std::byte byte_value(unsigned value)
{
    return std::byte{static_cast<unsigned char>(value)};
}

template<typename T>
T nth_value(
    nxtrt::buffer_chunks<const T> chunks,
    std::size_t index)
{
    for (auto chunk : chunks) {
        if (index < chunk.size())
            return chunk[index];
        index -= chunk.size();
    }
    throw nxtrt::buffer_error{"test chunk index out of range"};
}

struct counted_byte_frame
{
    nxtrt::byte_chunks<const std::byte> payload;

    static nxtrt::chop_scan_result<counted_byte_frame> scan(
        nxtrt::byte_chunks<const std::byte> bytes)
    {
        if (bytes.size() < 1)
            return nxtrt::chop_need_more{.minimum_buffered = 1};

        auto payload_size = static_cast<std::size_t>(
            std::to_integer<unsigned char>(nth_value(bytes, 0)));
        auto extent = payload_size + 1;
        if (bytes.size() < extent)
            return nxtrt::chop_need_more{.minimum_buffered = extent};

        return nxtrt::frame_chop<counted_byte_frame>{
            .extent = extent,
            .frame = counted_byte_frame{
                .payload = bytes.subspan(1, payload_size),
            },
        };
    }
};

struct counted_int_frame
{
    nxtrt::value_chunks<const int> payload;

    static nxtrt::chop_scan_result<counted_int_frame> scan(
        nxtrt::value_chunks<const int> values)
    {
        if (values.size() < 1)
            return nxtrt::chop_need_more{.minimum_buffered = 1};

        auto payload_size = static_cast<std::size_t>(
            nth_value(values, 0));
        auto extent = payload_size + 1;
        if (values.size() < extent)
            return nxtrt::chop_need_more{.minimum_buffered = extent};

        return nxtrt::frame_chop<counted_int_frame>{
            .extent = extent,
            .frame = counted_int_frame{
                .payload = values.subspan(1, payload_size),
            },
        };
    }
};

template<std::ranges::input_range Chops>
std::vector<std::string> counted_payload_texts(Chops && chops)
{
    auto out = std::vector<std::string>{};
    for (auto && item : chops)
        out.push_back(byte_value_chunks_text(item.frame.payload));
    return out;
}

template<std::ranges::input_range Chops>
std::vector<std::vector<int>> counted_int_payloads(Chops && chops)
{
    auto out = std::vector<std::vector<int>>{};
    for (auto && item : chops) {
        auto payload = std::vector<int>{};
        for (auto chunk : item.frame.payload) {
            for (auto value : chunk)
                payload.push_back(value);
        }
        out.push_back(std::move(payload));
    }
    return out;
}

inline nxtrt::task<void> check_byte_feed_ring_shape(
    nxtrt::feed<std::byte> & source)
{
    auto initial = co_await source.peek(3);
    expect(byte_value_chunks_text(initial) == "abc");
    expect(initial.chunk_count() == std::size_t{1});

    auto discarded = co_await source.discard(2);
    expect(value_count(discarded) == std::size_t{2});

    auto wrapped = co_await source.peek(4);
    expect(byte_value_chunks_text(wrapped) == "cdef");
    expect(wrapped.chunk_count() == std::size_t{2});

    auto out = std::vector<std::byte>{};
    auto sink = nxtrt::container_sink{out};
    auto streamed = co_await nxtrt::stream_all(source, sink);
    expect(streamed == std::size_t{4});
    expect(nxtrt::as_string_view(out) == "cdef");

    auto eof = co_await source.take();
    expect(!eof);
}

inline nxtrt::task<void> check_reel_chops_ring_feed(nxtrt::bytefeed & reader)
{
    auto frames = nxtrt::reel<std::byte, counted_byte_frame>{reader};

    auto first = co_await frames.peek(2);
    expect(counted_payload_texts(first) == std::vector<std::string>{"ab", "c"});
    expect(first.extent(1) == std::size_t{3});
    co_await frames.discard_prefix(first.extent(1));

    auto wrapped = co_await frames.peek(2);
    expect(reader.buffered().chunk_count() == std::size_t{2});
    expect(
        counted_payload_texts(wrapped | std::views::take(2))
        == std::vector<std::string>{"c", "de"});
    expect(
        nxtrt::chop_extent(wrapped | std::views::take(2))
        == std::size_t{5});

    co_await frames.discard_prefix(
        nxtrt::chop_extent(wrapped | std::views::take(2)));
    auto eof = co_await frames.peek();
    expect(eof.empty());
}

inline nxtrt::task<void> check_stock_reel_chops_ring_feed(nxtrt::feed<int> & source)
{
    auto frames = nxtrt::reel<int, counted_int_frame>{source};

    auto first = co_await frames.peek(2);
    expect(
        counted_int_payloads(first)
        == std::vector<std::vector<int>>{{10, 20}, {30}});
    expect(first.extent(1) == std::size_t{3});
    co_await frames.discard_prefix(first.extent(1));

    auto wrapped = co_await frames.peek(2);
    expect(source.buffered().chunk_count() == std::size_t{2});
    expect(
        counted_int_payloads(wrapped | std::views::take(2))
        == std::vector<std::vector<int>>{{30}, {40, 50}});
    expect(
        nxtrt::chop_extent(wrapped | std::views::take(2))
        == std::size_t{5});

    co_await frames.discard_prefix(
        nxtrt::chop_extent(wrapped | std::views::take(2)));
    auto eof = co_await frames.peek();
    expect(eof.empty());
}

inline nxtrt::task<const int *> peek_int_value(nxtrt::feed<int> & source)
{
    co_return co_await source.peek();
}

inline nxtrt::task<void> check_feed_one_methods(
    nxtrt::feed<int> & source)
{
    auto first = co_await source.peek_one();
    expect(first != nullptr);
    expect(*first == 7_i);
    expect(source.buffered_size() == std::size_t{1});

    auto taken = co_await source.take_one();
    expect(taken == 7_i);
    expect(source.buffered_size() == std::size_t{0});
}

inline nxtrt::task<int> take_one_int_value(nxtrt::feed<int> & source)
{
    co_return co_await source.take_one();
}

inline nxtrt::task<void> write_int_values(
    nxtrt::sink<int> & sink,
    int first,
    int second)
{
    co_await sink.write(first);
    co_await sink.write(second);
}

inline nxtrt::task<void> check_sink_buffers_until_flush(
    collecting_int_sink & sink)
{
    co_await sink.write(1);
    co_await sink.write(2);
    expect(sink.collected.empty());
    auto buffered = sink.buffered();
    expect(buffered.size() == std::size_t{2});
    expect(buffered.chunk_count() == std::size_t{1});
    co_await sink.flush();
}

inline nxtrt::task<void> check_sink_ring_buffer(collecting_int_sink & sink)
{
    co_await sink.write(1);
    co_await sink.write(2);
    co_await sink.write(3);
    co_await sink.write(4);
    expect(sink.collected == std::vector<int>{1, 2});

    co_await sink.write(5);

    auto buffered = sink.buffered();
    expect(buffered.size() == std::size_t{3});
    expect(buffered.chunk_count() == std::size_t{2});

    auto staged = std::vector<int>{};
    for (auto chunk : buffered) {
        for (auto value : chunk)
            staged.push_back(value);
    }
    expect(staged == std::vector<int>{3, 4, 5});

    co_await sink.flush();
}

inline nxtrt::task<void> check_range_source_lookahead(
    nxtrt::feed<int> & source)
{
    auto first = co_await source.peek();
    expect(first != nullptr);
    expect(*first == 5_i);
    auto taken = co_await source.take();
    expect(taken && *taken == 5_i);
    auto second = co_await source.take();
    expect(second && *second == 6_i);
}

inline nxtrt::task<void> discard_expected_prefix(nxtrt::feed<int> & source)
{
    co_await source.discard_all(1, 2);
}

inline nxtrt::task<void> discard_mismatched_prefix(nxtrt::feed<int> & source)
{
    co_await source.discard_all(1, 4);
}

inline nxtrt::task<std::optional<int>> take_int_value(
    nxtrt::feed<int> & source)
{
    co_return co_await source.take();
}

inline nxtrt::task<void> discard_past_eof(nxtrt::feed<int> & source)
{
    co_await source.discard_all(1, 2);
}

inline nxtrt::task<std::optional<int>> parse_digit_value(
    nxtrt::bytefeed & reader)
{
    try {
        auto byte = co_await reader.take_string_view(1);
        co_return byte.front() - '0';
    } catch (const nxtrt::end_of_stream &) {
        co_return std::nullopt;
    }
}

inline nxtrt::task<std::optional<int>> parse_summed_pair(nxtrt::feed<int> & reader)
{
    auto first = co_await reader.take();
    if (!first)
        co_return std::nullopt;

    auto second = co_await reader.take();
    if (!second)
        throw nxtrt::value_end_of_stream{"partial pair"};

    co_return *first + *second;
}

inline nxtrt::task<std::vector<nxtrt::http::server_sent_event>>
read_sse_events_from_response(nxtrt::bytefeed & reader)
{
    auto head = co_await nxtrt::http::read_response_head(reader);
    auto body = nxtrt::http::response_body_decoding_reader(reader, head);
    auto events = nxtrt::http::sse_event_parser(body);
    auto out = std::vector<nxtrt::http::server_sent_event>{};

    out.push_back(co_await events.take_one());
    auto end = co_await events.take();
    expect(!end);

    co_return out;
}

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

struct tool_batch_probe
{
    std::vector<int> delays;
    std::set<int> failures;
    std::vector<int> completed;
    int started = 0;
    int active = 0;
    int peak = 0;
    int settled = 0;
    int cancelled = 0;
    bool block = false;
    bool self_cancel = false;
};

inline nxtrt::task<nxtai::tools::tool_result>
run_tool_batch_probe(tool_batch_probe & state, int id)
{
    ++state.started;
    ++state.active;
    state.peak = std::max(state.peak, state.active);
    expect(nxtrt::require_current_firm().child_count() == 0);

    struct active_guard
    {
        tool_batch_probe & state;

        ~active_guard()
        {
            --state.active;
            ++state.settled;
        }
    } guard{state};

    if (state.block) {
        while (!nxtrt::task_stop_requested())
            co_await nxtrt::yield();
        // Cleanup must await actual settlement, not just request stop.
        co_await nxtrt::yield();
        co_await nxtrt::yield();
        ++state.cancelled;
        throw nxtrt::operation_cancelled{};
    }
    for (int i = 0; i < state.delays[id]; ++i)
        co_await nxtrt::yield();
    nxtrt::throw_if_stop_requested();
    if (state.self_cancel) {
        ++state.cancelled;
        throw nxtrt::operation_cancelled{};
    }
    state.completed.push_back(id);
    if (state.failures.contains(id))
        throw nxtrt::runtime_error{"probe failure " + std::to_string(id)};
    co_return nxtai::tools::tool_result{.output = std::to_string(id)};
}

struct batch_probe_tool : echo_tool
{
    tool_batch_probe * state;

    nxtrt::task<nxtai::tools::tool_result> run(parameters args) const
    {
        return run_tool_batch_probe(*state, std::stoi(args.text));
    }
};

inline std::vector<nxtai::tools::function_call> tool_batch_probe_calls(int count)
{
    auto calls = std::vector<nxtai::tools::function_call>{};
    for (int i = 0; i < count; ++i) {
        auto id = std::to_string(i);
        calls.push_back({
            .call_id = id,
            .name = "echo",
            .arguments = "{\"text\":\"" + id + "\"}",
        });
    }
    return calls;
}

inline nxtrt::task<int> read_ambient_int_after_yield()
{
    co_await nxtrt::yield();
    co_return nxtrt::env_require<ambient_int_key>();
}

inline nxtrt::task<int> read_ambient_int()
{
    co_return nxtrt::env_require<ambient_int_key>();
}

inline nxtrt::task<void> record_after_yield(std::vector<int> & events, int value)
{
    events.push_back(value * 10 + 1);
    co_await nxtrt::yield();
    events.push_back(value * 10 + 2);
}

inline nxtrt::task<int> root_task_probe(bool & had_firm)
{
    had_firm = nxtrt::current_firm() != nullptr;
    co_return 9;
}

inline nxtrt::task<void>
record_next_wire_value(
    nxtrt::wire<int> & events,
    std::vector<int> & out)
{
    auto value = co_await events.next();
    if (value)
        out.push_back(*value);
}

inline nxtrt::task<void>
record_closed_wire(nxtrt::wire<int> & events, bool & finished)
{
    auto value = co_await events.next();
    expect(!value);
    finished = true;
}

inline nxtrt::task<void>
flush_wire(nxtrt::wire<int> & events, bool & flushed)
{
    co_await events.flush();
    flushed = true;
}

inline nxtrt::task<void>
send_wire_value(nxtrt::wire<int> & events, int value, bool & sent)
{
    sent = co_await events.send(value);
}

inline nxtrt::task<void>
record_after_bell(
    nxtrt::bell & ready,
    std::vector<int> & out,
    int value)
{
    co_await ready;
    out.push_back(value);
}

inline nxtrt::task<void> record_current_firm(
    std::vector<nxtrt::firm *> & firms)
{
    co_await nxtrt::yield();
    firms.push_back(nxtrt::current_firm());
}

inline nxtrt::task<void> record_current_task_id_after_yield(
    std::vector<nxtrt::task_id> & ids)
{
    co_await nxtrt::yield();
    auto * deck = nxtrt::current_deck();
    expect(deck != nullptr);
    ids.push_back(deck->current_task_id());
}

inline nxtrt::task<void> probe_fork_deck_overflow_body(
    nxtrt::firm & scope,
    std::vector<int> & events,
    bool & overflowed,
    std::size_t & child_count_after_failure)
{
    try {
        scope.fork(record_after_yield(events, 7));
    } catch (const nxtrt::runtime_error & e) {
        overflowed =
            std::string_view{e.what()}.contains("deck task table is full");
        child_count_after_failure =
            nxtrt::require_current_firm().child_count();
    }
    co_return;
}

struct fork_deck_overflow_root
{
    std::vector<int> * events = nullptr;
    bool * overflowed = nullptr;
    std::size_t * child_count_after_failure = nullptr;

    nxtrt::task<void> operator()() const
    {
        return nxtrt::with_firm([&](nxtrt::firm & scope) {
            return probe_fork_deck_overflow_body(
                scope, *events, *overflowed, *child_count_after_failure);
        });
    }
};

inline nxtrt::task<void> record_current_int_game(
    std::vector<nxtrt::game<int> *> & games)
{
    co_await nxtrt::yield();
    games.push_back(nxtrt::current_game<int>());
}

inline nxtrt::task<void> post_game_event(int event)
{
    co_await nxtrt::require_current_game<int>().sync({
        .post = {event},
    });
}

inline nxtrt::task<void> wait_for_game_event(
    int event,
    std::vector<int> & seen)
{
    auto selected = co_await nxtrt::require_current_game<int>().sync({
        .post = {},
        .wait = [event](int x) { return x == event; },
    });
    seen.push_back(selected);
}

inline nxtrt::task<void> halt_game_event_once(
    int event,
    std::vector<int> & seen)
{
    auto selected = co_await nxtrt::require_current_game<int>().sync({
        .post = {},
        .wait = [event](int x) { return x != event; },
        .halt = [event](int x) { return x == event; },
    });
    seen.push_back(selected);
    nxtrt::require_current_firm().stop();
}

inline nxtrt::task<void> wait_for_never_game_event(bool & cancelled)
{
    try {
        (void)co_await nxtrt::require_current_game<int>().sync({
            .post = {},
            .wait = [](int) { return false; },
        });
    } catch (const nxtrt::operation_cancelled &) {
        cancelled = true;
    }
}

enum class ttt_kind
{
    move,
    x_win,
    o_win,
    draw,
};

struct ttt_event
{
    ttt_kind kind = ttt_kind::move;
    char player = 'X';
    int row = 0;
    int col = 0;

    friend bool operator==(ttt_event const &, ttt_event const &) = default;
};

struct ttt_board
{
    std::array<char, 9> cells{};

    [[nodiscard]] char at(int row, int col) const
    {
        return cells[static_cast<std::size_t>(row * 3 + col)];
    }

    void place(ttt_event event)
    {
        cells[static_cast<std::size_t>(event.row * 3 + event.col)] =
            event.player;
    }

    [[nodiscard]] bool full() const
    {
        return std::ranges::all_of(cells, [](char cell) {
            return cell != '\0';
        });
    }

    [[nodiscard]] bool wins(char player) const
    {
        auto line = [&](int a, int b, int c) {
            return cells[static_cast<std::size_t>(a)] == player
                && cells[static_cast<std::size_t>(b)] == player
                && cells[static_cast<std::size_t>(c)] == player;
        };
        return line(0, 1, 2) || line(3, 4, 5) || line(6, 7, 8)
            || line(0, 3, 6) || line(1, 4, 7) || line(2, 5, 8)
            || line(0, 4, 8) || line(2, 4, 6);
    }
};

inline ttt_event ttt_move(char player, int row, int col)
{
    return ttt_event{
        .kind = ttt_kind::move,
        .player = player,
        .row = row,
        .col = col,
    };
}

inline nxtrt::task<void> ttt_enforce_turns()
{
    for (;;) {
        co_yield nxtrt::sync_spec<ttt_event>{
            .post = {},
            .wait = [](ttt_event const & event) {
                return event.kind == ttt_kind::move && event.player == 'X';
            },
            .halt = [](ttt_event const & event) {
                return event.kind == ttt_kind::move && event.player == 'O';
            },
        };
        co_yield nxtrt::sync_spec<ttt_event>{
            .post = {},
            .wait = [](ttt_event const & event) {
                return event.kind == ttt_kind::move && event.player == 'O';
            },
            .halt = [](ttt_event const & event) {
                return event.kind == ttt_kind::move && event.player == 'X';
            },
        };
    }
}

inline nxtrt::task<void> ttt_square_taken(int row, int col)
{
    auto is_square = [row, col](ttt_event const & event) {
        return event.kind == ttt_kind::move
            && event.row == row
            && event.col == col;
    };
    co_yield nxtrt::sync_spec<ttt_event>{
        .post = {},
        .wait = is_square,
    };
    for (;;) {
        co_yield nxtrt::sync_spec<ttt_event>{
            .post = {},
            .halt = is_square,
        };
    }
}

inline nxtrt::task<void> ttt_detect_end(
    ttt_board & board,
    std::vector<ttt_event> & events)
{
    for (;;) {
        auto event = co_yield nxtrt::sync_spec<ttt_event>{
            .post = {},
            .wait = [](ttt_event const & event) {
                return event.kind == ttt_kind::move;
            },
        };
        events.push_back(event);
        board.place(event);

        if (board.wins(event.player)) {
            auto kind = event.player == 'X' ? ttt_kind::x_win : ttt_kind::o_win;
            auto selected = co_yield ttt_event{.kind = kind};
            events.push_back(selected);
            nxtrt::require_current_firm().stop();
            co_return;
        }
        if (board.full()) {
            auto selected = co_yield ttt_event{.kind = ttt_kind::draw};
            events.push_back(selected);
            nxtrt::require_current_firm().stop();
            co_return;
        }
    }
}

inline nxtrt::task<void> ttt_x_script()
{
    auto moves = std::array{
        ttt_move('X', 1, 1),
        ttt_move('X', 0, 1),
        ttt_move('X', 2, 1),
    };
    for (auto move : moves) {
        co_yield move;
    }
}

inline nxtrt::task<void> ttt_o_ai()
{
    for (;;) {
        co_yield nxtrt::sync_spec<ttt_event>{
            .post = {},
            .wait = [](ttt_event const & event) {
                return event.kind == ttt_kind::move && event.player == 'X';
            },
        };
        co_yield nxtrt::sync_spec<ttt_event>{
            .post = {
                ttt_move('O', 1, 1),
                ttt_move('O', 0, 0),
                ttt_move('O', 0, 2),
                ttt_move('O', 2, 0),
                ttt_move('O', 2, 2),
                ttt_move('O', 0, 1),
                ttt_move('O', 1, 0),
                ttt_move('O', 1, 2),
                ttt_move('O', 2, 1),
            },
        };
    }
}

inline nxtrt::task<bool> read_task_stop_after_yield()
{
    co_await nxtrt::yield();
    co_return nxtrt::task_stop_requested();
}

inline nxtrt::task<bool> shielded_child_stop_state()
{
    co_return co_await nxtrt::shield(read_task_stop_after_yield());
}

inline nxtrt::task<void> await_manual_token(nxtrt::coin_t token)
{
    co_await nxtrt::op::manual{token};
}

inline nxtrt::task<void> shielded_manual_token(nxtrt::coin_t token)
{
    co_await nxtrt::shield(await_manual_token(token));
}

inline nxtrt::task<void> throw_after_yield(std::vector<int> & events, int value)
{
    events.push_back(value * 10 + 1);
    co_await nxtrt::yield();
    throw nxtrt::runtime_error{"firm child boom"};
}

inline nxtrt::task<int> value_after_yield(int value)
{
    co_await nxtrt::yield();
    co_return value;
}

inline nxtrt::task<std::string> string_after_yield(std::string value)
{
    co_await nxtrt::yield();
    co_return value;
}

inline nxtrt::task<int> value_after_two_yields_or_stop(
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

inline nxtrt::task<int> throw_int_after_yield()
{
    co_await nxtrt::yield();
    throw nxtrt::runtime_error{"firm child int boom"};
}

inline nxtrt::task<int> tuple_wait_for_stop(
    std::vector<int> & events, int value, int * starts = nullptr)
{
    if (starts != nullptr)
        ++*starts;
    while (!nxtrt::stop_requested())
        co_await nxtrt::yield();
    events.push_back(value);
    throw nxtrt::operation_cancelled{};
}

inline nxtrt::task<int> tuple_frame_context(std::vector<int> & events)
{
    events.push_back(
        nxtrt::require_current_firm().child_count() == 0 ? 3 : -3);
    co_return 17;
}

struct returned_deeds_firm : nxtrt::firm
{
    using firm::firm;

    static nxtrt::task<void> empty_child()
    {
        co_await nxtrt::yield();
    }

    nxtrt::task<std::tuple<nxtrt::deed<int>, nxtrt::deed<void>>>
    operator()()
    {
        auto a = fork(value_after_yield(41));
        auto b = fork(empty_child());
        co_await join();
        co_return std::tuple{std::move(a), std::move(b)};
    }
};

struct external_result_firm : nxtrt::firm
{
    explicit external_result_firm(int & target)
        : target(target)
    {}

    int & target;

    nxtrt::task<nxtrt::deed<int>> operator()()
    {
        auto child = fork(value_after_yield(64));
        child.store_result_in(target);
        co_await join();
        co_return std::move(child);
    }
};

inline nxtrt::task<nxtrt::deed<int>> fork_external_result_into(int & target)
{
    co_return co_await external_result_firm{target};
}

struct firm_result_value
{
    explicit firm_result_value(int value)
        : value(value)
    {}

    firm_result_value(const firm_result_value &) = delete;
    firm_result_value & operator=(const firm_result_value &) = delete;

    firm_result_value(firm_result_value && other) noexcept
        : value(std::exchange(other.value, -1))
    {}

    firm_result_value & operator=(firm_result_value &&) = delete;

    int value = 0;
};

inline nxtrt::task<firm_result_value> firm_result_value_after_yield(int value)
{
    co_await nxtrt::yield();
    co_return firm_result_value{value};
}

struct external_result_cell_firm : nxtrt::firm
{
    explicit external_result_cell_firm(
        nxtrt::deed_result_storage<firm_result_value> & target)
        : target(target)
    {}

    nxtrt::deed_result_storage<firm_result_value> & target;

    nxtrt::task<nxtrt::deed<firm_result_value>> operator()()
    {
        auto child = fork(firm_result_value_after_yield(71));
        child.store_result_in(target);
        co_await join();
        co_return std::move(child);
    }
};

struct pooled_result_cell_firm : nxtrt::firm
{
    explicit pooled_result_cell_firm(
        nxtrt::deed_result_storage_pool_ref<firm_result_value> & pool)
        : pool(&pool)
    {}

    nxtrt::deed_result_storage_pool_ref<firm_result_value> * pool = nullptr;

    nxtrt::task<nxtrt::deed<firm_result_value>> operator()()
    {
        auto & target = pool->borrow();
        auto child = fork(firm_result_value_after_yield(72));
        child.store_result_in(target);
        co_await join();
        co_return std::move(child);
    }
};

inline nxtrt::task<nxtrt::deed<firm_result_value>>
fork_external_result_cell_into(
    nxtrt::deed_result_storage<firm_result_value> & target)
{
    co_return co_await external_result_cell_firm{target};
}

inline nxtrt::task<nxtrt::deed<firm_result_value>>
fork_pooled_result_cell_into(
    nxtrt::deed_result_storage_pool_ref<firm_result_value> & pool)
{
    co_return co_await pooled_result_cell_firm{pool};
}

struct external_result_root
{
    int * target = nullptr;

    nxtrt::task<nxtrt::deed<int>> operator()()
    {
        return fork_external_result_into(*target);
    }
};

struct external_result_cell_root
{
    nxtrt::deed_result_storage<firm_result_value> * target = nullptr;

    nxtrt::task<nxtrt::deed<firm_result_value>> operator()()
    {
        return fork_external_result_cell_into(*target);
    }
};

struct pooled_result_cell_root
{
    nxtrt::deed_result_storage_pool_ref<firm_result_value> * pool =
        nullptr;

    nxtrt::task<nxtrt::deed<firm_result_value>> operator()()
    {
        return fork_pooled_result_cell_into(*pool);
    }
};

inline nxtrt::task<void> record_stop_state_after_yield(
    std::vector<int> & events,
    int value)
{
    co_await nxtrt::yield();
    events.push_back(nxtrt::stop_requested() ? value : -value);
}

inline nxtrt::task<void>
hosted_firm_stop_body(nxtrt::firm & scope, std::vector<int> & events)
{
    scope.fork(record_stop_state_after_yield(events, 4));
    events.push_back(100);
    co_await nxtrt::yield();
    co_await scope.join();
}

inline nxtrt::task<void> hosted_firm_stop_probe(std::vector<int> & events)
{
    co_await nxtrt::with_firm([&](nxtrt::firm & scope) {
        return hosted_firm_stop_body(scope, events);
    });
}

inline nxtrt::task<void> record_stop_state_after_two_yields(
    std::vector<int> & events,
    int value)
{
    co_await nxtrt::yield();
    co_await nxtrt::yield();
    events.push_back(nxtrt::stop_requested() ? value : -value);
}

inline nxtrt::task<void> record_task_stop_state_after_yield(
    std::vector<int> & events,
    int value)
{
    co_await nxtrt::yield();
    events.push_back(nxtrt::task_stop_requested() ? value : -value);
}

// A custom awaitable exercising task::splice_onto. When `ready` holds it
// resolves through await_ready() and never suspends; on a miss it delegates
// its slow path to a real task spliced as the awaiter continuation. This is
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
take_three_buffered_bytes(nxtrt::bytefeed & reader, std::vector<int> & events)
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
check_bytefeed_chunk_peek(nxtrt::bytefeed & reader)
{
    auto initial = co_await reader.peek_chunks(3);
    expect(byte_value_chunks_text(initial) == "abc");
    expect(initial.chunk_count() == std::size_t{1});

    auto taken = co_await reader.take_string_view(2);
    expect(taken == "ab"sv);

    auto rest = co_await reader.peek_chunks(2);
    expect(byte_value_chunks_text(rest) == "cd");
    expect(rest.chunk_count() == std::size_t{1});
}

inline nxtrt::task<void>
write_three_buffered_bytes(nxtrt::bytesink & writer, std::vector<int> & events)
{
    events.push_back(1);
    co_await nxtrt::write(writer, "a"sv);
    events.push_back(2);
    co_await nxtrt::write(writer, "b"sv);
    events.push_back(3);
    co_await nxtrt::write(writer, "c"sv);
    events.push_back(4);
}

inline nxtrt::task<void> flush_writer(nxtrt::bytesink & writer)
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

inline nxtrt::task<int> map_over_manual_wish(nxtrt::coin_t token)
{
    co_return co_await nxtrt::map(
        nxtrt::op::manual{token}, [] { return 7; });
}

void declare_runtime_firm_tests();
void declare_runtime_firm_stop_tests();
void declare_runtime_buffer_tests();
void declare_runtime_io_tests();

} // namespace nxt::test
