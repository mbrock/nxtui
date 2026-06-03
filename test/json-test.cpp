#include <nxt/json.hpp>
#include <nxtrt.hpp>

#include "test.hpp"

#include <string>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

std::vector<nxt::json::token> read_all_json_tokens(std::string_view input)
{
    auto deck = nxtrt::deck{};
    return deck.sync_wait([input]() -> nxtrt::task<std::vector<nxt::json::token>> {
        auto reader = nxt::json::string_reader{.input = input};
        auto out = std::vector<nxt::json::token>{};
        while (auto token = co_await nxt::json::read_token(reader))
            out.push_back(std::move(*token));
        co_return out;
    });
}

static boost::ut::suite json_tests{
    "JSON", [] {
    "tokenizes punctuation and simple values"_test = [] {
        auto tokens = read_all_json_tokens(R"({"ok":true,"n":12,null:false})");
        expect(tokens.size() == std::size_t{13});
        expect(tokens[0].kind == nxt::json::token_kind::object_begin);
        expect(tokens[1].text == "ok");
        expect(tokens[3].kind == nxt::json::token_kind::boolean);
        expect(tokens[3].boolean);
        expect(tokens[5].text == "n");
        expect(tokens[7].kind == nxt::json::token_kind::number);
        expect(tokens[7].text == "12");
        expect(tokens[9].kind == nxt::json::token_kind::null);
        expect(tokens[11].kind == nxt::json::token_kind::boolean);
        expect(!tokens[11].boolean);
        expect(tokens[12].kind == nxt::json::token_kind::object_end);
    };

    "decodes string escapes"_test = [] {
        auto tokens = read_all_json_tokens(R"(["a\nb","quote: \"","snowman: \u2603"])");
        expect(tokens.size() == std::size_t{7});
        expect(tokens[1].text == "a\nb");
        expect(tokens[3].text == "quote: \"");
        expect(tokens[5].text == "snowman: \xE2\x98\x83");
    };

    "decodes surrogate pairs"_test = [] {
        auto tokens = read_all_json_tokens(R"(["\ud83d\ude80"])");
        expect(tokens.size() == std::size_t{3});
        expect(tokens[1].text == "\xF0\x9F\x9A\x80");
    };

    "reports malformed strings"_test = [] {
        auto failed = false;
        try {
            (void) read_all_json_tokens(R"(["\q"])");
        } catch (const nxt::json::parse_error &) {
            failed = true;
        }
        expect(failed);
    };

    "writes escaped strings"_test = [] {
        auto w = nxt::json::writer{};
        w.character('[');
        w.string("a\nb \"c\"");
        w.character(',');
        w.string(std::string_view{"x\001y", 3});
        w.character(']');
        expect(w.out == R"(["a\nb \"c\"","x\u0001y"])");
    };
}};

} // namespace nxt::test
