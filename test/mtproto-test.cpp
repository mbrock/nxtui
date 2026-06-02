#include <nxt/mt/crypto.hpp>
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

int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F')
        return 10 + ch - 'A';
    throw std::runtime_error{"bad hex digit"};
}

std::vector<std::byte> hex(std::string_view input)
{
    auto out = std::vector<std::byte>{};
    out.reserve(input.size() / 2);
    auto high = -1;
    for (auto ch : input) {
        if (ch == ' ' || ch == '\n')
            continue;
        if (high < 0) {
            high = hex_digit(ch);
            continue;
        }
        out.push_back(static_cast<std::byte>((high << 4) | hex_digit(ch)));
        high = -1;
    }
    if (high >= 0)
        throw std::runtime_error{"odd hex input length"};
    return out;
}

template<std::size_t N>
std::array<std::byte, N> counting_bytes(std::uint8_t first = 0)
{
    auto out = std::array<std::byte, N>{};
    for (auto i = std::size_t{0}; i < out.size(); i++)
        out[i] = std::byte{static_cast<std::uint8_t>(first + i)};
    return out;
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

        "derives MT auth key metadata and message keys"_test = [] {
            auto key_data = counting_bytes<256>();
            auto key = nxt::mt::make_auth_key(key_data);

            expect(std::ranges::equal(
                key.aux_hash,
                hex("4916d6bdb7f78e68")));
            expect(std::ranges::equal(
                key.id,
                hex("32d1586ea457dfc8")));

            auto nonce = counting_bytes<32>();
            expect(std::ranges::equal(
                nxt::mt::calc_new_nonce_hash(key, nonce, 1),
                hex("c2ced2b33e593a55d27f4a5dabee7c67")));

            auto plaintext = counting_bytes<32>(16);
            auto msg_key = nxt::mt::message_key(
                key,
                plaintext,
                nxt::mt::sender::client);
            expect(std::ranges::equal(
                msg_key,
                hex("fbfa5fa94e2a70f3ad96dd24f7ad36b5")));

            auto key_iv = nxt::mt::derive_aes_key_iv(
                key,
                msg_key,
                nxt::mt::sender::client);
            expect(std::ranges::equal(
                key_iv.key,
                hex("36bce969c89677d9cadd87de515f83a5"
                    "265d2f17274cb43de11122996338e5f2")));
            expect(std::ranges::equal(
                key_iv.iv,
                hex("4808d5e2c25ecf23d82aab1b1aae0376"
                    "e073e9f175db200548fb5432d4980271")));
        };

        "encrypts and decrypts MT padded payloads in caller buffers"_test = [] {
            auto key = nxt::mt::make_auth_key(counting_bytes<256>());
            auto plaintext = counting_bytes<32>(16);
            auto payload = std::vector<std::byte>(
                nxt::mt::encrypted_payload_size(plaintext));

            nxt::mt::encrypt_padded(
                plaintext,
                key,
                nxt::mt::sender::client,
                payload);

            expect(std::ranges::equal(
                std::span<const std::byte>{payload}.first(8),
                key.id));
            auto opened = std::vector<std::byte>(plaintext.size());
            auto view = nxt::mt::decrypt_padded(
                payload,
                key,
                nxt::mt::sender::client,
                opened);
            expect(std::ranges::equal(view, plaintext));
        };

        "encrypts auth exchange data with hash using caller scratch"_test = [] {
            auto server_nonce = counting_bytes<16>(16);
            auto new_nonce = counting_bytes<32>();
            auto key_iv = nxt::mt::temp_aes_key_iv(server_nonce, new_nonce);
            auto data = counting_bytes<44>(32);
            auto padding = counting_bytes<16>(160);
            auto ciphertext = std::vector<std::byte>(
                nxt::mt::encrypted_data_with_hash_size(data.size()));

            nxt::mt::encrypt_data_with_hash(
                data,
                key_iv.key,
                key_iv.iv,
                padding,
                ciphertext);

            auto opened = std::vector<std::byte>(ciphertext.size());
            auto view = nxt::mt::decrypt_data_with_hash(
                ciphertext,
                key_iv.key,
                key_iv.iv,
                opened);

            expect(std::ranges::equal(view, data));
            expect(
                nxt::mt::server_salt(new_nonce, server_nonce)
                == 0x1010101010101010LL);
        };
    }};

} // namespace nxt::test
