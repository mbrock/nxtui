#include <nxtio/buffers.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::test {

using namespace boost::ut;
using namespace std::literals;

class scripted_char_source
{
public:
    explicit scripted_char_source(std::vector<std::string_view> chunks)
        : chunks_(std::move(chunks))
    {
    }

    nxt::task<std::size_t> read_some(std::span<char> dst)
    {
        if (chunk_ == chunks_.size())
            co_return 0;

        auto src = chunks_[chunk_];
        auto rest = src.substr(offset_);
        auto n = std::min(dst.size(), rest.size());
        std::ranges::copy(rest.substr(0, n), dst.begin());

        offset_ += n;
        if (offset_ == src.size()) {
            ++chunk_;
            offset_ = 0;
        }

        co_return n;
    }

private:
    std::vector<std::string_view> chunks_;
    std::size_t chunk_ = 0;
    std::size_t offset_ = 0;
};

class char_sink
{
public:
    nxt::task<> write_all(std::string_view text)
    {
        text_ += text;
        co_return;
    }

    [[nodiscard]] const std::string & text() const noexcept
    {
        return text_;
    }

private:
    std::string text_;
};

template<typename Exception, typename Fn>
bool throws_exception(Fn fn)
{
    try {
        fn();
    } catch (const Exception &) {
        return true;
    }
    return false;
}

suite buffers_tests = [] {
    "reader keeps read-ahead in its own buffer"_test = [] {
        auto chunks = std::array{"hello world"sv};
        nxt::io::string_source source{std::span{chunks}};
        std::array<std::byte, 16> storage{};
        nxt::io::byte_reader reader{source, std::span{storage}};

        auto first = nxt::sync_wait(reader.take(5));
        expect(nxt::io::as_string_view(first) == "hello"sv);
        expect(nxt::io::as_string_view(reader.buffered()) == " world"sv);

        reader.toss(1);
        auto second = nxt::sync_wait(reader.take(5));
        expect(nxt::io::as_string_view(second) == "world"sv);
        expect(reader.buffered().empty());
    };

    "reader rebases borrowed storage while taking a delimiter"_test = [] {
        auto chunks = std::array{"ab"sv, "cd"sv, "ef\nrest"sv};
        nxt::io::string_source source{std::span{chunks}};
        std::array<std::byte, 5> storage{};
        nxt::io::byte_reader reader{source, std::span{storage}};

        auto prefix = nxt::sync_wait(reader.take(2));
        expect(nxt::io::as_string_view(prefix) == "ab"sv);

        auto line = nxt::sync_wait(reader.take_until("\n"sv));
        expect(nxt::io::as_string_view(line) == "cdef"sv);
        expect(reader.buffered().empty());
    };

    "reader reports delimiter units that exceed buffer policy"_test = [] {
        auto chunks = std::array{"abcdef"sv};
        nxt::io::string_source source{std::span{chunks}};
        std::array<std::byte, 4> storage{};
        nxt::io::byte_reader reader{source, std::span{storage}};

        expect(throws_exception<nxt::io::buffer_error>([&] {
            nxt::sync_wait(reader.take_until("\n"sv));
        }));
    };

    "reader can adapt a char-oriented source"_test = [] {
        scripted_char_source source{{"abc", "def"}};
        std::array<std::byte, 8> storage{};
        nxt::io::byte_reader reader{source, std::span{storage}};

        auto text = nxt::sync_wait(reader.take(6));
        expect(nxt::io::as_string_view(text) == "abcdef"sv);
    };

    "writer buffers borrowed storage until flush"_test = [] {
        nxt::io::string_sink sink;
        std::array<std::byte, 8> storage{};
        nxt::io::byte_writer writer{sink, std::span{storage}};

        nxt::sync_wait(writer.write_all("hello"sv));
        expect(sink.text().empty());
        expect(nxt::io::as_string_view(writer.buffered()) == "hello"sv);

        nxt::sync_wait(writer.flush());
        expect(sink.text() == "hello");
        expect(writer.buffered().empty());
    };

    "writer exposes writable space with advance and undo"_test = [] {
        nxt::io::string_sink sink;
        std::array<std::byte, 8> storage{};
        nxt::io::byte_writer writer{sink, std::span{storage}};

        auto writable = nxt::sync_wait(writer.writable(4));
        auto text = std::as_bytes(std::span{"abcd"sv});
        std::ranges::copy(text, writable.begin());
        writer.advance(4);
        writer.undo(1);

        nxt::sync_wait(writer.flush());
        expect(sink.text() == "abc");
    };

    "writer can adapt a char-oriented sink"_test = [] {
        char_sink sink;
        std::array<std::byte, 4> storage{};
        nxt::io::byte_writer writer{sink, std::span{storage}};

        nxt::sync_wait(writer.write_all("hello world"sv));
        nxt::sync_wait(writer.flush());
        expect(sink.text() == "hello world");
    };

    "reader streams exact bytes into writer"_test = [] {
        auto chunks = std::array{"hello "sv, "world!"sv};
        nxt::io::string_source source{std::span{chunks}};
        nxt::io::string_sink sink;
        std::array<std::byte, 6> read_storage{};
        std::array<std::byte, 4> write_storage{};
        nxt::io::byte_reader reader{source, std::span{read_storage}};
        nxt::io::byte_writer writer{sink, std::span{write_storage}};

        nxt::sync_wait(reader.stream_exact(writer, 11));
        nxt::sync_wait(writer.flush());

        expect(sink.text() == "hello world");
        expect(nxt::io::as_string_view(reader.buffered()) == "!"sv);
    };

    "reader and writer check cancellation before suspending io"_test = [] {
        auto chunks = std::array{"hello"sv};
        nxt::io::string_source source{std::span{chunks}};
        nxt::io::string_sink sink;
        std::stop_source stop;
        std::array<std::byte, 8> read_storage{};
        std::array<std::byte, 8> write_storage{};
        nxt::io::byte_reader reader{
            source, std::span{read_storage}, stop.get_token()};
        nxt::io::byte_writer writer{
            sink, std::span{write_storage}, stop.get_token()};

        stop.request_stop();

        expect(throws_exception<nxt::io::operation_cancelled>([&] {
            nxt::sync_wait(reader.fill(1));
        }));
        expect(throws_exception<nxt::io::operation_cancelled>([&] {
            nxt::sync_wait(writer.write_all("hello world"sv));
        }));
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
