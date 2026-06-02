#include <nxt/mt/auth.hpp>
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

        "encrypts Telegram padded RSA blocks in caller buffers"_test = [] {
            auto modulus = hex(R"(
                e8bb3305c0b52c6cf2afdf7637313489e63e05268e5badb601af417786472e5f
                93b85438968e20e6729a301c0afc121bf7151f834436f7fda680847a66bf64
                accec78ee21c0b316f0edafe2f41908da7bd1f4a5107638eeb67040ace472
                a14f90d9f7c2b7def99688ba3073adb5750bb02964902a359fe745d8170e3
                6876d4fd8a5d41b2a76cbff9a13267eb9580b2d06d10357448d20d9da21
                91cb5d8c93982961cdfdeda629e37f1fb09a0722027696032fe61ed663d
                b7a37f6f263d370f69db53a0dc0a1748bdaaff6209d5645485e6e001d1
                953255757e4b8e42813347b11da6ab500fd0ace7e6dfa3736199ccaf939
                7ed0745a427dcfa6cd67bcb1acff3
            )");
            auto key = nxt::mt::public_key{
                .modulus = modulus,
                .exponent = 65537,
            };
            auto plaintext = std::vector<std::byte>(144, std::byte{0x61});
            auto random = std::vector<std::byte>(
                nxt::mt::rsa_required_random_bytes(),
                std::byte{0});
            auto encrypted = std::vector<std::byte>(
                nxt::mt::rsa_padded_block_size);

            nxt::mt::rsa_encrypt_padded(
                plaintext,
                key,
                random,
                encrypted);

            expect(std::ranges::equal(
                encrypted,
                hex(R"(
                    bf68719e836806b040cd261ecaf66eb3c4ba19f3bbea3031b2e6cf29167bab64
                    7201d101b291dc5b716a42e789a38d947fe59e9bcce8f30ef46a946743ea8b6b
                    abbce7fc0afc46b802aa453e83471d82a4dfad83f971f350b4b4fb474cd1c48f
                    df427e4b5fecce9ec3178ae7dac3985856fdefa21d6fdc5e0e0fd8a57bc4f515
                    80d637d372be8d87c9aa3fde8e6f8287bcb3be846aadcdd59465375479e248f6
                    2ed438f9804fbe36d41ca906243a5f740f3937949aa149ba8a8b8e68b3f3e1e3
                    cd3f946387520e21eee55845e1f015a919a22f6a72bfaecd2cae946c91983b41
                    f9ffabe97963bbde8f30eaf5fd3c5b8cecab8711bd269e441b6084f385726ff0
                )")));
        };

        "derives MT public key fingerprints without PEM parsing"_test = [] {
            auto modulus = hex(R"(
                c8c11d635691fac091dd9489aedced2932aa8a0bcefef05fa800892d9b52ed03
                200865c9e97211cb2ee6c7ae96d3fb0e15aeffd66019b44a08a240cfdd2868
                a85e1f54d6fa5deaa041f6941ddf302690d61dc476385c2fa655142353cb4
                e4b59f6e5b6584db76fe8b1370263246c010c93d011014113ebdf987d093f
                9d37c2be48352d69a1683f8f6e6c2167983c761e3ab169fde5daaa1212
                3fa1beab621e4da5935e9c198f82f35eae583a99386d8110ea6bd1abb0
                f568759f62694419ea5f69847c43462abef858b4cb5edc84e7b9226cd7
                bd7e183aa974a712c079dde85b9dc063b8a5c08e8f859c0ee5dcd824
                c7807f20153361a7f63cfd2a433a1be7f5
            )");
            auto scratch = std::array<std::byte, 264>{};

            auto key = nxt::mt::auth::make_public_key(
                modulus,
                65537,
                scratch);

            expect(key.fingerprint == 0xb25898df208d2603ULL);
            auto keys = std::array{key};
            auto fingerprints = std::array<std::uint64_t, 2>{
                0,
                0xb25898df208d2603ULL,
            };
            expect(
                nxt::mt::auth::select_public_key(keys, fingerprints)
                == &keys[0]);
        };

        "writes req_pq_multi and decodes resPQ into borrowed views"_test = [] {
            auto nonce = hex("4e44b426241e8b839153122d44585ac6");
            auto request = std::array<std::byte, 20>{};
            auto writer = nxt::mt::byte_writer{request};
            nxt::mt::auth::write_req_pq_multi(writer, nonce);
            expect(std::ranges::equal(
                writer.written(),
                hex("f18e7ebe4e44b426241e8b839153122d44585ac6")));

            auto response = hex(R"(
                632416054e44b426241e8b839153122d44585ac665ba0b393e1094329eda2c42
                d62833030819546f942a11278d00000015c4b51c0300000003268d20df9858b2
                029f4ba16d109296216be86c022bb4c3
            )");
            auto fingerprints = std::array<std::uint64_t, 4>{};
            auto decoded = nxt::mt::auth::decode_res_pq(response, fingerprints);

            expect(std::ranges::equal(decoded.nonce, nonce));
            expect(std::ranges::equal(
                decoded.server_nonce,
                hex("65ba0b393e1094329eda2c42d6283303")));
            expect(std::ranges::equal(decoded.pq, hex("19546f942a11278d")));
            expect(
                decoded.server_public_key_fingerprints.size()
                == std::size_t{3});
            expect(
                decoded.server_public_key_fingerprints[0]
                == 0xb25898df208d2603ULL);

            auto [p, q] = nxt::mt::auth::factor_pq(decoded.pq);
            expect(p == 0x44b2e50dULL);
            expect(q == 0x5e63ac81ULL);
        };

        "writes auth req_DH_params into caller buffers"_test = [] {
            auto nonce = counting_bytes<16>();
            auto server_nonce = counting_bytes<16>(16);
            auto p = hex("01");
            auto q = hex("02");
            auto encrypted = bytes("abc");
            auto storage = std::array<std::byte, 56>{};
            auto writer = nxt::mt::byte_writer{storage};

            nxt::mt::auth::write_req_dh_params(
                writer,
                nonce,
                server_nonce,
                p,
                q,
                0x0102030405060708ULL,
                encrypted);

            expect(writer.size() == nxt::mt::auth::req_dh_params_size(
                p,
                q,
                encrypted));
            expect(std::ranges::equal(
                writer.written(),
                hex(R"(
                    bee412d7000102030405060708090a0b0c0d0e0f
                    101112131415161718191a1b1c1d1e1f01010000
                    01020000080706050403020103616263
                )")));
        };
    }};

} // namespace nxt::test
