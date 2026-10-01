// SPDX-License-Identifier: AGPL-3.0-or-later
#include <wisp/reader.hpp>

#include "test.hpp"

namespace wisp::test {
namespace {

using namespace boost::ut;

struct reading
{
    heap h;
    evaluator vm{h};

    word one(std::string_view source)
    {
        reader r{h, vm, source};
        const auto value = r.next().value();
        expect(!r.next().has_value());
        return value;
    }

    std::vector<word> list(word x)
    {
        std::vector<word> items;
        while (x != nil) {
            if (tag_of(x) != tag::duo)
                throw std::runtime_error("expected proper list");
            const auto [car, cdr] = h.read<tag::duo>(x);
            items.push_back(car);
            x = cdr;
        }
        return items;
    }

    void
    bad(std::string_view source, std::size_t offset, std::string_view why)
    {
        reader r{h, vm, source};
        try {
            while (r.next()) {
            }
            expect(false) << "accepted malformed source: " << source;
        } catch (const read_error & error) {
            expect(error.offset == offset) << "source: " << source;
            expect(std::string_view{error.what()}.contains(why))
                << error.what();
            expect(r.position() <= source.size());
        }
    }
};

static suite reader_tests{
    "WISP READER", [] {
        "incremental forms distinguish NIL, zero, and clean EOF"_test = [] {
            reading m;
            reader empty{m.h, m.vm, ""};
            expect(!empty.next().has_value());
            expect(empty.position() == 0u);
            reader r{m.h, m.vm, " nil() 0; ignored ]\r\t\n\ft ;final"};
            expect(r.position() == 0u);
            expect(r.next() == std::optional{nil});
            expect(r.position() == 4u);
            expect(r.next() == std::optional{nil});
            expect(r.position() == 6u);
            expect(r.next() == std::optional{word{0}});
            expect(r.position() == 8u);
            expect(r.next() == std::optional{t});
            expect(r.position() == 24u);
            expect(!r.next().has_value());
            expect(r.position() == 31u);
            expect(!r.next().has_value());
            expect(m.one("(a; no separator needed\n b)") != nil);
        };

        "unsigned digit prefixes and signed-symbol quirks"_test = [] {
            reading m;
            reader r{
                m.h,
                m.vm,
                "007 1073741823 0000000000 -7 +7 123abc 1.2 0x10"};
            for (auto want :
                 {word{7},
                  word{1073741823},
                  word{0},
                  m.vm.intern("-7"),
                  m.vm.intern("+7"),
                  word{123},
                  m.vm.intern("ABC"),
                  word{1},
                  m.vm.intern(".2"),
                  word{0},
                  m.vm.intern("X10")})
                expect(r.next() == std::optional{want});
            expect(!r.next().has_value());
            m.bad("1073741824", 9, "overflow");
            m.bad("9999999999", 9, "overflow");
            m.bad("00000000000", 10, "overflow");
        };

        "ASCII case folding, keyword identity, and uninterned symbols"_test =
            [] {
                reading m;
                expect(m.one("miXeD") == m.vm.intern("MIXED"));
                expect(m.one("|x\\y|") == m.vm.intern("|X\\Y|"));
                expect(
                    m.one("~20220314.8njafj7wjf")
                    == m.vm.intern("~20220314.8NJAFJ7WJF"));
                const auto key = m.one(":nil");
                expect(key == m.vm.keyword("NIL") && key != nil);
                expect((m.h.get<tag::sym, field::val>(key) == key));
                m.h.set<tag::sym, field::val>(key, 9);
                expect(m.one(":NIL") == key);
                expect((m.h.get<tag::sym, field::val>(key) == key));
                expect(m.one("::x") == m.vm.keyword(":X"));
                expect(m.one(":") == m.vm.keyword(""));
                const auto a = m.one("#:nIl"), b = m.one("#:NIL");
                expect(a != b && a != nil);
                expect(
                    (m.h.read<tag::sym>(a)
                     == row<tag::sym>{
                         m.h.get<tag::sym, field::str>(a),
                         nil,
                         nah,
                         nil,
                         nil}));
                expect(
                    m.h.v08slice(m.h.get<tag::sym, field::str>(a))
                    == "NIL");
                const auto colon = m.one("#:123:x");
                expect(
                    m.h.v08slice(m.h.get<tag::sym, field::str>(colon))
                    == "123:X");
                const auto blank = m.one("#:");
                expect(m.h.v08slice(m.h.get<tag::sym, field::str>(blank))
                           .empty());
            };

        "package-qualified names share evaluator interning"_test = [] {
            reading m;
            expect(m.one("wisp:nil") == nil);
            expect(m.one("WiSp:T") == t);
            expect(m.one("wisp:foo") == m.vm.intern("FOO"));
            expect(m.one("wisp:") == m.vm.intern(""));
            const auto plain = m.one("keyword:plain");
            expect(plain == m.vm.keyword("PLAIN"));
            expect((m.h.get<tag::sym, field::val>(plain) == nah));
            expect(m.one(":plain") == plain);
            expect((m.h.get<tag::sym, field::val>(plain) == plain));
            const auto key = m.one("key:x");
            expect(key != m.vm.intern("X") && key != m.vm.keyword("X"));
            expect(
                (m.h.get<tag::sym, field::pkg>(key)
                 == m.vm.find_package("KEY")));
            expect(m.one("KEY:X") == key);
            m.bad("missing:x", 0, "no such package");
            m.bad("wisp::x", 5, "colon in symbol name");
            m.bad("wisp:x:y", 6, "colon in symbol name");
        };

        "lists, vectors, and the dot-at-list-tail rule"_test = [] {
            reading m;
            expect(m.one("()") == nil);
            expect(m.h.v32slice(m.one("[]")).empty());
            expect(
                m.list(m.one("(3 nil t)")) == std::vector<word>{3, nil, t});
            const auto pair = m.one("(3 8 . 21)");
            const auto [first, rest] = m.h.read<tag::duo>(pair);
            expect(first == 3u);
            expect((m.h.read<tag::duo>(rest) == row<tag::duo>{8, 21}));
            expect(m.one("(.x)") == m.vm.intern("X"));
            expect(
                (m.h.read<tag::duo>(m.one("(a .b)"))
                 == row<tag::duo>{m.vm.intern("A"), m.vm.intern("B")}));
            expect(
                m.list(m.one("(1 . (2 5))")) == std::vector<word>{1, 2, 5});
            expect(m.one(".") == m.vm.intern("."));
            const auto mixed = m.one("[1 (2 . 3) [5] .foo]");
            const auto xs = m.h.v32slice(mixed);
            expect(xs.size() == 4u);
            expect(xs[0] == 1u && xs[3] == m.vm.intern(".FOO"));
            expect((m.h.read<tag::duo>(xs[1]) == row<tag::duo>{2, 3}));
            expect(m.h.v32slice(xs[2]).size() == 1u);
            expect(m.h.v32slice(xs[2])[0] == 5u);
        };

        "reader shorthand expands to WISP symbols without context checks"_test =
            [] {
                reading m;
                for (auto [source, op] :
                     {std::pair{"'x", "QUOTE"},
                      {"`x", "BACKQUOTE"},
                      {",x", "UNQUOTE"},
                      {",@x", "UNQUOTE-SPLICING"},
                      {"#'x", "FUNCTION"},
                      {", @x", "UNQUOTE"}}) {
                    const auto xs = m.list(m.one(source));
                    expect(xs.size() == 2u);
                    expect(xs[0] == m.vm.intern(op));
                    expect(
                        xs[1]
                        == m.vm.intern(
                            source == std::string_view{", @x"} ? "@X"
                                                               : "X"));
                }
                const auto outer = m.list(m.one("'`(,x ,@xs)"));
                expect(outer[0] == m.vm.intern("QUOTE"));
                const auto backquote = m.list(outer[1]);
                expect(backquote[0] == m.vm.intern("BACKQUOTE"));
                const auto forms = m.list(backquote[1]);
                expect(
                    m.list(forms[0])
                    == std::vector<word>{
                        m.vm.intern("UNQUOTE"), m.vm.intern("X")});
                expect(
                    m.list(forms[1])
                    == std::vector<word>{
                        m.vm.intern("UNQUOTE-SPLICING"),
                        m.vm.intern("XS")});
            };

        "strings use only newline, quote, and backslash escapes"_test = [] {
            reading m;
            expect(
                m.h.v08slice(m.one(R"("a\n\"\\;é🙂")")) == "a\n\"\\;é🙂");
            const std::string raw{"\"a\0\t\r\nb\"", 8};
            expect(
                m.h.v08slice(m.one(raw))
                == std::string_view{"a\0\t\r\nb", 6});
            expect(m.h.v08slice(m.one("\"\"")).empty());
            m.bad(R"("\t")", 2, "escape");
            m.bad(R"("\r")", 2, "escape");
            m.bad(R"("\0")", 2, "escape");
            m.bad(R"("\x41")", 2, "escape");
        };

        "characters consume one Unicode scalar, not a named token"_test =
            [] {
                reading m;
                reader r{m.h, m.vm, "#\\space #\\) #\\é #\\🙂 #\\ "};
                expect(r.next() == std::optional{immediate(tag::chr, 's')});
                expect(r.position() == 3u);
                expect(r.next() == std::optional{m.vm.intern("PACE")});
                for (word c : {0x29, 0xe9})
                    expect(
                        r.next() == std::optional{immediate(tag::chr, c)});
                expect(
                    r.next()
                    == std::optional{immediate(tag::chr, 0x1f642)});
                expect(r.next() == std::optional{immediate(tag::chr, ' ')});
                expect(!r.next().has_value());
                expect(
                    m.one(std::string_view{"#\\\0", 3})
                    == immediate(tag::chr, 0));
                expect(
                    m.one("#\\\xf4\x8f\xbf\xbf")
                    == immediate(tag::chr, 0x10ffff));
            };

        "Unicode symbol ranges and ASCII-only case folding"_test = [] {
            reading m;
            reader r{m.h, m.vm, "éa λz 🙂 \u2003"};
            expect(r.next() == std::optional{m.vm.intern("éA")});
            expect(r.position() == 3u);
            expect(r.next() == std::optional{m.vm.intern("λZ")});
            expect(r.next() == std::optional{m.vm.intern("🙂")});
            expect(r.next() == std::optional{m.vm.intern("\u2003")});
            expect(!r.next().has_value());
            expect(m.one("\u02af") == m.vm.intern("\u02af"));
            m.bad("\u02b0", 0, "unexpected character");
            m.bad("a\u0301", 1, "unexpected character");
            m.bad("\u00a0", 0, "unexpected character");
            m.bad("#é", 1, "dispatch");
            for (auto bytes :
                 {"\x80",
                  "\xc0\x80",
                  "\xc2",
                  "\xe0\x80\x80",
                  "\xed\xa0\x80",
                  "\xf0\x80\x80\x80",
                  "\xf4\x90\x80\x80",
                  "\xf5\x80\x80\x80",
                  "\xe2(x",
                  "\xff"}) {
                m.bad(bytes, 0, "UTF-8");
                m.bad(std::string{"\""} + bytes + '"', 1, "UTF-8");
                m.bad(std::string{";"} + bytes + '\n', 1, "UTF-8");
            }
        };

        "malformed delimiters, dispatch, truncation, and non-skipped space"_test =
            [] {
                reading m;
                for (auto source :
                     {"(",
                      "[",
                      "'",
                      "`",
                      ",",
                      ",@",
                      "#",
                      "#\\",
                      "#'",
                      "\"",
                      "\"abc",
                      "\"abc\\",
                      "(1 . 2",
                      "[1",
                      "([",
                      "(1 .",
                      "(1 .;EOF"})
                    m.bad(source, std::string_view{source}.size(), "EOF");
                for (auto source : {")", "]", "{", "}", "\t", "\r", "\v"})
                    m.bad(source, 0, "unexpected character");
                m.bad("(]", 1, "unexpected character");
                m.bad("[)", 1, "unexpected character");
                m.bad("(.)", 2, "unexpected character");
                m.bad("(a . b c)", 7, "expected ')'");
                m.bad("a\tb", 1, "unexpected character");
                for (auto source : {"#(", "#|", "#;", "#x"})
                    m.bad(source, 1, "dispatch");
            };

        "owned input survives host mutation, heap growth, and collection"_test =
            [] {
                reading m;
                std::string source = "keep";
                reader host{m.h, m.vm, source};
                source.assign("lost");
                expect(host.next() == std::optional{m.vm.intern("KEEP")});

                const auto input = m.h.newv08("[\"first\" #:x] \"second\"");
                reader guest{m.h, m.vm, m.h.v08slice(input)};
                m.h.newv08(
                    std::string(100000, 'x')); // invalidate borrowed input
                root form{m.h, guest.next().value()};
                m.h.collect(); // also discards the unrooted original input
                const auto xs = m.h.v32slice(form.get());
                expect(xs.size() == 2u);
                expect(m.h.v08slice(xs[0]) == "first");
                expect(
                    m.h.v08slice(m.h.get<tag::sym, field::str>(xs[1]))
                    == "X");
                const auto second = guest.next().value();
                expect(m.h.v08slice(second) == "second");
                expect(!guest.next().has_value());
            };

        "long lists and deeply nested forms use no recursive native stack"_test =
            [] {
                reading m;
                std::string source = "(";
                for (unsigned i = 0; i < 12000; ++i)
                    source += "7 ";
                source += ')';
                const auto xs = m.list(m.one(source));
                expect(xs.size() == 12000u);
                expect(
                    std::ranges::all_of(xs, [](word x) { return x == 7; }));

                source.clear();
                for (unsigned i = 0; i < 6000; ++i)
                    source += "(['";
                source += '9';
                for (unsigned i = 0; i < 6000; ++i)
                    source += "])";
                auto x = m.one(source);
                for (unsigned i = 0; i < 6000; ++i) {
                    const auto list = m.list(x);
                    expect(list.size() == 1u);
                    const auto vector = m.h.v32slice(list[0]);
                    expect(vector.size() == 1u);
                    const auto quote = m.list(vector[0]);
                    expect(quote.size() == 2u);
                    expect(quote[0] == m.vm.intern("QUOTE"));
                    x = quote[1];
                }
                expect(x == 9u);
                m.bad(std::string(30000, '['), 30000, "EOF");
            };
    }};

} // namespace
} // namespace wisp::test
