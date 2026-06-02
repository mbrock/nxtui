#include <nxt/mt/message.hpp>
#include <nxt/mt/tl.hpp>
#include <nxt/mt/transport.hpp>
#include <nxtrt/deck.hpp>
#include <nxtrt/mtproto.hpp>

#include "test.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::test {

using namespace boost::ut;
using namespace std::literals;

namespace {

std::span<const std::byte> bytes(std::string_view text)
{
    return std::as_bytes(std::span{text});
}

std::string text(std::span<const std::byte> bytes)
{
    return std::string{
        reinterpret_cast<const char *>(bytes.data()),
        bytes.size()};
}

template<std::ranges::viewable_range Range>
auto text_source(Range && chunks, std::span<std::byte> storage)
{
    return nxtrt::byte_span_feed{
        std::forward<Range>(chunks),
        storage,
    };
}

template<typename Source>
nxtrt::task<std::string> read_frame_text(Source & source)
{
    auto frame = co_await nxtrt::mtproto::read_abridged_frame(source);
    co_return text(frame);
}

nxtrt::task<void>
write_frame(nxtrt::bytesink & sink, std::span<const std::byte> payload)
{
    co_await nxtrt::mtproto::write_abridged_frame(sink, payload);
}

} // namespace

static suite mtproto_tests{
    "MTProto", [] {
        "writes into caller-owned buffers and reports overflow"_test = [] {
            auto storage = std::array<std::byte, 4>{};
            auto writer = nxt::mt::byte_writer{storage};
            writer.put_u8(0x12);
            writer.put_le(0x00abcdef, 3);

            expect(writer.size() == std::size_t{4});
            expect(std::to_integer<unsigned>(storage[0]) == 0x12);
            expect(std::to_integer<unsigned>(storage[1]) == 0xef);
            expect(std::to_integer<unsigned>(storage[2]) == 0xcd);
            expect(std::to_integer<unsigned>(storage[3]) == 0xab);

            auto overflowed = false;
            try {
                writer.put_u8(0);
            } catch (const nxt::mt::protocol_error &) {
                overflowed = true;
            }
            expect(overflowed);
        };

        "encodes and decodes TL bytes as borrowed spans"_test = [] {
            auto storage = std::array<std::byte, 8>{};
            auto writer = nxt::mt::byte_writer{storage};
            nxt::mt::tl::write_bytes(writer, bytes("abc"));

            auto reader = nxt::mt::tl::reader{writer.written()};
            auto value = reader.bytes();
            expect(text(value) == "abc");
            expect(reader.empty());
            expect(writer.size() == std::size_t{4});
        };

        "encodes and decodes plain messages without owning the body"_test = [] {
            auto body = bytes("ping");
            auto storage = std::array<std::byte, 24>{};
            auto writer = nxt::mt::byte_writer{storage};

            nxt::mt::write_plain_message(
                writer,
                nxt::mt::plain_message_view{
                    .message_id = 0x0102030405060708,
                    .body = body,
                });

            auto message = nxt::mt::read_plain_message(writer.written());
            expect(message.message_id == 0x0102030405060708ULL);
            expect(text(message.body) == "ping");
        };

        "encodes and decodes abridged frames over borrowed buffers"_test = [] {
            auto payload = bytes("abcd");
            auto storage = std::array<std::byte, 8>{};
            auto writer = nxt::mt::byte_writer{storage};

            nxt::mt::write_abridged_frame(writer, payload);
            expect(writer.size() == std::size_t{5});

            auto reader = nxt::mt::byte_reader{writer.written()};
            auto frame = nxt::mt::read_abridged_frame(reader);
            auto * decoded = std::get_if<nxt::mt::abridged_payload>(&frame);
            expect(decoded != nullptr);
            expect(text(decoded->bytes) == "abcd");
            expect(reader.empty());
        };

        "reads abridged runtime frames from a bytefeed"_test = [] {
            auto deck = nxtrt::deck{};
            auto chunks = std::array{"\x81\0\0\0\x01"sv, "abcd"sv};
            auto storage = std::array<std::byte, 16>{};
            auto source = text_source(chunks, std::span{storage});

            auto payload = deck.sync_wait(read_frame_text(source));

            expect(payload == "abcd");
        };

        "writes abridged runtime frames to a container sink"_test = [] {
            auto deck = nxtrt::deck{};
            auto out = std::vector<std::byte>{};
            auto sink = nxtrt::container_sink{out};

            deck.sync_wait(write_frame(sink, bytes("abcd")));

            expect(out.size() == std::size_t{5});
            expect(std::to_integer<unsigned>(out[0]) == 1);
            expect(text(std::span<const std::byte>{out}.subspan(1)) == "abcd");
        };
    }};

} // namespace nxt::test
