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
#include <ranges>
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

std::string text(nxtrt::byte_chunks<const std::byte> chunks)
{
    auto out = std::string{};
    for (auto chunk : chunks)
        out += text(chunk);
    return out;
}

std::byte byte_value(unsigned value)
{
    return std::byte{static_cast<unsigned char>(value)};
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

template<std::ranges::input_range Chops>
std::vector<std::string> abridged_payload_texts(Chops && chops)
{
    auto out = std::vector<std::string>{};
    for (auto && item : chops) {
        if (item.frame.is_payload())
            out.push_back(text(item.frame.payload));
    }
    return out;
}

nxtrt::task<void>
check_abridged_reel_ring_feed(nxtrt::bytefeed & source)
{
    auto frames = nxtrt::mtproto::abridged_reel{source};

    auto first = co_await frames.peek(2);
    expect(
        abridged_payload_texts(first)
        == std::vector<std::string>{"abcd", "efgh"});
    expect(first.extent(1) == std::size_t{5});
    co_await frames.discard_prefix(first.extent(1));

    auto wrapped = co_await frames.peek(2);
    expect(source.buffered().chunk_count() == std::size_t{2});
    expect(
        abridged_payload_texts(wrapped | std::views::take(2))
        == std::vector<std::string>{"efgh", "ijkl"});
    expect(
        nxtrt::chop_extent(wrapped | std::views::take(2))
        == std::size_t{10});

    co_await frames.discard_prefix(
        nxtrt::chop_extent(wrapped | std::views::take(2)));
    auto eof = co_await frames.peek();
    expect(eof.empty());
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

        "projects abridged transport frames as reel chops"_test = [] {
            auto bytes = std::array{
                byte_value(0x81),
                byte_value(0x02),
                byte_value(0x03),
                byte_value(0x04),
                byte_value(2),
                byte_value('a'),
                byte_value('b'),
                byte_value('c'),
                byte_value('d'),
                byte_value('e'),
                byte_value('f'),
                byte_value('g'),
                byte_value('h'),
            };
            auto all = std::span<const std::byte>{
                bytes.data(),
                bytes.size(),
            };
            auto spans = std::array{
                all.first(5),
                all.subspan(5),
            };
            auto chunks = nxtrt::byte_chunks<const std::byte>{
                std::span{spans},
            };

            auto frames =
                nxtrt::chop<std::byte, nxtrt::mtproto::abridged_frame_view>(
                    chunks);
            auto it = frames.begin();

            expect(it != frames.end());
            expect(it->frame.is_quick_ack());
            expect(
                *it->frame.quick_ack_token
                == nxt::mt::byte_swap32(0x04030281));
            ++it;

            expect(it != frames.end());
            expect(it->frame.is_payload());
            expect(text(it->frame.payload) == "abcdefgh");
            expect(it->extent == std::size_t{9});
            ++it;

            expect(it == frames.end());
            expect(frames.extent() == std::size_t{13});
        };

        "peeks abridged runtime frames as borrowed reel views"_test = [] {
            auto deck = nxtrt::deck{};
            auto chunks = std::array{
                "\x01" "abcd" "\x01" "efgh" "\x01" "ijkl"sv,
            };
            auto storage = std::array<std::byte, 10>{};
            auto source = text_source(chunks, std::span{storage});

            deck.sync_wait(check_abridged_reel_ring_feed(source));
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

        "writes and reads runtime plain messages through abridged frames"_test = [] {
            auto deck = nxtrt::deck{};
            auto out = std::vector<std::byte>{};
            auto sink = nxtrt::container_sink{out};
            auto last_message_id = std::optional<std::uint64_t>{};
            auto message_storage = std::array<std::byte, 32>{};

            deck.sync_wait(nxtrt::mtproto::write_plain_abridged_frame(
                sink,
                bytes("ping"),
                last_message_id,
                message_storage,
                1'693'436'740'000'000'000ULL));

            expect(last_message_id.has_value());
            expect(out.size() == std::size_t{25});
            expect(std::to_integer<unsigned>(out[0]) == 6);

            auto source_chunks =
                std::array{std::span<const std::byte>{out}};
            auto source_storage = std::array<std::byte, 64>{};
            auto source = nxtrt::byte_span_feed{
                source_chunks,
                std::span{source_storage},
            };

            auto message =
                deck.sync_wait(nxtrt::mtproto::read_plain_abridged_frame(source));

            expect(message.message_id == *last_message_id);
            expect(text(message.body) == "ping");
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

        "builds req_DH_params from resPQ using borrowed scratch"_test = [] {
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
            auto key_scratch = std::array<std::byte, 264>{};
            auto key = nxt::mt::auth::make_public_key(
                modulus,
                65537,
                key_scratch);
            auto keys = std::array{key};
            auto state = nxt::mt::auth::exchange_state{};
            auto nonce = hex("4e44b426241e8b839153122d44585ac6");
            auto begin_storage = std::array<std::byte, 20>{};
            auto begin_writer = nxt::mt::byte_writer{begin_storage};
            nxt::mt::auth::begin(state, nonce, begin_writer);

            auto response = hex(R"(
                632416054e44b426241e8b839153122d44585ac665ba0b393e1094329eda2c42
                d62833030819546f942a11278d00000015c4b51c0300000003268d20df9858b2
                029f4ba16d109296216be86c022bb4c3
            )");
            auto random = hex(R"(
                b9dce68b05ef760fa7edfefeff45aaa8afbac11dc3d333bc3132fd16ab816d63
                ed93c5bef9d0452add8164a2d5df5804277ee5a06fd4523372707ddbd8106d03
                766d76fb8bec672bdcddcd225f7766b83663b32a0fda1055175c5582edd10430
                937666be4fd15510ba5f19aa645973b6e4e9270efac25b58741635fe84dd0af0
                7a4686f750bf34de1073f1e7fa24e9b01a76e537504bd52b8195e5b78c9af2ba
                a982454e1a99eeae0f35944089ad12726d2433a2c18c9698a725364f9c4e939c
                e4f1aee3891e58b85de90c88cc2eaef5db1841a594c0edc13cb4b7480a7e564f
                e892f82282d03ed07eb5ceac6644247bb137241166fe194756dfcffd68c6c345
            )");
            auto fingerprints = std::array<std::uint64_t, 4>{};
            auto inner = std::array<std::byte, 128>{};
            auto encrypted = std::array<std::byte, 256>{};
            auto request = std::array<std::byte, 320>{};
            auto writer = nxt::mt::byte_writer{request};

            nxt::mt::auth::receive_res_pq(
                state,
                response,
                keys,
                random,
                fingerprints,
                inner,
                encrypted,
                writer);

            expect(
                state.current_phase
                == nxt::mt::auth::phase::awaiting_server_dh_params);
            expect(std::ranges::equal(
                state.new_nonce,
                std::span<const std::byte>{random}.first(32)));
            expect(std::ranges::equal(
                writer.written(),
                hex(R"(
                    bee412d74e44b426241e8b839153122d44585ac665ba0b393e1094329eda2c42
                    d62833030444b2e50d000000045e63ac8100000003268d20df9858b2fe000100
                    7ec37ca8a84aa1b26d21bc8ac28b261ffa57b44e29f0d6722261e9b436059cc8
                    0ae9768a3ae4fbefe46cfbb76b88a1f80a1ebd95ae5d17bf655ed1015755e04c
                    483a01cf4094a0830864054a71a0ac8a5ec34d6b24a69bf66c9654b32a8c65b0
                    302718351b28f72a9a49610d5259b6edb6da37acc5fedc47d1a09c58df2c7ecc
                    bfaf54dfe123ebc253d9069f74e8be128051e5d280b3c9a5e8d3c6da344cb737
                    4a6d410d4e088cc0eda3d8b1108ba4f4a85d79fbd2758000723780bc5459f59f
                    d1cea1b511b77cc1411781d3feb57b14a97726cf3d2146cf43e648a69ff9cb5d
                    48a31f543bd5bc3a023cf382d86d36bbfbbcb5e4a136acee25fd8e3e597e714d
                )")));

            auto server_dh_params = hex(R"(
                5c07e8d04e44b426241e8b839153122d44585ac665ba0b393e1094329eda2c42
                d6283303fe500200fd064e91012ade621b26a48ac7dc8b2c8670ed67092a00fe
                8c936483e4b02822c3cc655aaffe00542e311df5abdaa645b1da85ca50a6c7b0
                e7cc7cb2b23d42c84e288bb3b5cfe313e1ebafe19833916df4d1f58dba62e0ac
                49cac17a31b8b0d57d43eefda546d67e80e311c4b213adec9635c73f75a18ffb
                26fb71391523bd5ddfcc8be51b36d6b2552394c511ec935d53811a981baca62a
                2b58cbfe96f1b35e118e5e17456994aea931839925c4578f281f3f129d28026e
                c80224617a9ca8c615a12fba9c53e774476567f07b01a59d2e6635e39c16dc0a
                54679f3b54b0482f1cbeac821147d93d7365f4e23fb5794eb5fd4ffdc6456638
                ea32f641f49ee705e7b0da71cb75753e2f4f80d5af07edb017948f332e34a9c5
                886b0c86281e0e7228d5a652a9faaf819f7686c099186169aaa377c136fac57b
                69b7f7b383aaece652f8dcb14e0dfb23e2a65330307a74c31c508cc504450fa2
                08eee14d8bbead1c1f90ccfc183ae1d3345c62424ea3477776204e8fe69efbb6
                a27b168913d3babaca30aa1c9589d6655b2ad4cd59f67e9b3957ab3270d70afa
                b9bd488a6c5f39ca739ca8947def00cdb8812152731710f5108235775a019d3b
                4986d6b720b05167b4ee731a10a29fc1e03c42e99d8ff5cf64f45070c2f5ce48
                5ea5fddc281728b6e4d0dea561c9097e3f8a54b055b0c069a9f8207520f6429e
                b5225c985e3379f2cf6754f56d414fcd00d502e69223b911b915978e0890a9ef
                128715b828bf3fda3fee6c7b9b2621d971a6f7820f89f4c4c2ab29dec00007c3
                ec6cead64f7f5802d5e6a4a16a185cfbfced5351fa68380e
            )");
            auto server_random = hex(R"(
                8fc3605a4604cbb5461fdeff439c761150083cdd502550558e92c730d46c9caf
                0b1b2d64d2c264942c50d98694fff604fdd2bd87f2cafb719bc55e65a1f60b08
                809660a650721c40d56fc9c792df1d463aad1718c6924b7bdffbe395f14633d3
                3fc38ce47c18a1561b83a5c66d29f9e292637127471c3baab0028ae42796b689
                e53a7f9ab5f0ee6d3fb658d847c1abca509fc4ed0d45edbb1c946488910d8d78
                fa0767255b57a7c3898da8d26625bde40c5a0e80b581408ecd95a17d396dc757
                4a8ed3cbc4c085197ffaad29c18e577eb292aa8b98caa92efd6f9536049b5a7d
                efc861e270eca90c55b9585405cb96f3e6ea754850b09e7a59ba5fd92d357982
                915d39752aaa2ec16b6cbde6a6c33971
            )");
            auto decrypted = std::array<std::byte, 768>{};
            auto g_b = std::array<std::byte, 256>{};
            auto auth_key = std::array<std::byte, 256>{};
            auto client_inner = std::array<std::byte, 320>{};
            auto client_encrypted = std::array<std::byte, 384>{};
            auto set_client = std::array<std::byte, 420>{};
            auto set_writer = nxt::mt::byte_writer{set_client};

            nxt::mt::auth::receive_server_dh_params(
                state,
                server_dh_params,
                server_random,
                1'693'436'740,
                decrypted,
                g_b,
                auth_key,
                client_inner,
                client_encrypted,
                set_writer);

            expect(
                state.current_phase
                == nxt::mt::auth::phase::awaiting_dh_gen);
            expect(state.server_salt == 4'459'407'212'920'268'508LL);
            expect(state.time_offset == 0);
            expect(state.has_key);
            expect(std::ranges::equal(
                set_writer.written(),
                hex(R"(
                    1f5f04f54e44b426241e8b839153122d44585ac665ba0b393e1094329eda2c42
                    d6283303fe500100def448d48c608480bab65df3f8990be8011f7b415a6f8113
                    617bea749b8b0ea6a937987b18cc4dcce8197efdcf8d6ec6af7fc3364b4945df
                    77e4a1ae9db7acea4abcd73247edb36bde20fc969c1d55717277afe0bc31a9ee
                    99f7d822f91fa2dc69c868a19511b162d55e0814d0292b7708b67d57eb045693
                    49d5a20ffe85c0141fc17e9bbbaf207bef56e66decda718c52c45273f868c2ef
                    f89bb06355cd515fbfe123d719b244234867d2889c9d0e4436ba644076e5014a
                    78af60b2f0e1b30285f4f71539bcf8c506ccafd62cfcd1b040fe5e35bb30e519
                    ad56d753100f604e3ea5d02409d74dd3ab0861227410f1e13591cf2a638347e6
                    c6d0bcae14e0e8753313b51daee40a67407b5cc8b213856a290a0c7b6cda9ff9
                    c58d69faaf6a748cff05512b69f1380f7a36843edecdc764048bc16d9808f353
                    a9caf6d49ca8b717c8f6de037518a444931a7da2b80f16d0
                )")));

            nxt::mt::auth::receive_dh_gen(
                state,
                hex(R"(
                    34f7cb3b4e44b426241e8b839153122d44585ac665ba0b393e1094329eda2c42
                    d628330313b781a0de4ab6bc7ab414cbe13f9f86
                )"));

            expect(state.current_phase == nxt::mt::auth::phase::complete);
            expect(std::ranges::equal(
                state.key.data,
                hex(R"(
                    7582e48ad36cd6eef7944ac9bd7027de9ee3202543b68850ac01e1221350f717
                    4e6c3771c9d86b3075f777539c23d053e9da9a1510d49e8fa0ad76a016ce28bf
                    e3543dde69959bc682dab762b95a36629a8438e65baa53cc79b551c23d555c76
                    75a36f4ece90882ece497d28a903409b780a8a80516cb0f8534fee3a67530beb
                    2b1929626e07c2a052c4870b18b0a626606ca05cb13668a65aee3fa32cbebf1b
                    3a56532138cb22c017cac44a292021902eea9b9f906c6be19c9203c7bb3ebc5f
                    1b2044d0a90cb008f7248c3ae4449e0895b6090abb04c24131c2948bd27d879e
                    cb934e50a46671f987653385ab388e4fa1ddd4c95743111e08bf11fef1f8f739
                )")));
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
