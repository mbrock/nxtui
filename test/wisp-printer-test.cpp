// SPDX-License-Identifier: AGPL-3.0-or-later
#include <wisp/printer.hpp>

#include "test.hpp"

namespace wisp::test {
namespace {

using namespace boost::ut;
using namespace std::string_literals;

word package(heap & h, std::string_view name)
{
    return h.make<tag::pkg>({h.newv08(name), nil, nil});
}

word symbol(heap & h, std::string_view name, word pkg = nil)
{
    return h.make<tag::sym>({h.newv08(name), pkg, nah, nil, nil});
}

static suite printer_tests{
    "WISP PRINTING", [] {
        "signed fixnums and system immediates"_test = [] {
            heap h;
            expect(print(h, fixnum(0)) == "0");
            expect(print(h, fixnum(109)) == "109");
            expect(print(h, fixnum(-37)) == "-37");
            expect(print(h, fixnum(-1)) == "-1");
            expect(print(h, fixnum(min_fixnum)) == "-1073741824");
            expect(print(h, fixnum(max_fixnum)) == "1073741823");
            expect(print(h, nil) == "NIL");
            expect(print(h, t) == "T");
            expect(print(h, nah) == "#<NAH>");
            expect(print(h, top) == "#<TOP>");
            expect(print(h, zap) == "#<ZAP>");
            expect(print(h, immediate(tag::sys, 13)) == "#<sys 13>");
        };

        "strings escape only quote, backslash, and newline"_test = [] {
            heap h;
            expect(print(h, h.newv08("")) == "\"\"");
            expect(print(h, h.newv08("hello")) == "\"hello\"");
            const auto bytes = "a\"b\\c\n\r\t\0\x01\x7f\xc3\xa9"s;
            expect(
                print(h, h.newv08(bytes))
                == "\"a\\\"b\\\\c\\n\r\t\0\x01\x7f\xc3\xa9\""s);
            expect(print(h, h.newv08("\\n")) == "\"\\\\n\"");
        };

        "symbols retain spelling and use fixed WISP package context"_test =
            [] {
                heap h;
                auto base = package(h, "WISP");
                auto keywords = package(h, "KEYWORD");
                auto keys = package(h, "KEY");
                auto other = package(h, "OTHER");
                expect(print(h, symbol(h, "FOO", base)) == "FOO");
                expect(print(h, symbol(h, "FOO", keywords)) == ":FOO");
                expect(
                    print(h, symbol(h, "~20220314.X", keys))
                    == "~20220314.X");
                expect(print(h, symbol(h, "FOO", other)) == "OTHER:FOO");
                expect(print(h, symbol(h, "FOO")) == "#:FOO");
                expect(
                    print(h, symbol(h, "Mixed spelling", base))
                    == "Mixed spelling");
                expect(
                    print(h, symbol(h, "A\0B"s, other)) == "OTHER:A\0B"s);
                expect(print(h, base) == "<package>");
            };

        "proper and dotted lists, nesting, and vectors"_test = [] {
            heap h;
            auto proper = h.cons(1, h.cons(2, h.cons(3, nil)));
            expect(print(h, proper) == "(1 2 3)");
            expect(print(h, h.cons(1, 2)) == "(1 . 2)");
            expect(print(h, h.cons(1, h.cons(2, 3))) == "(1 2 . 3)");
            expect(print(h, h.cons(h.cons(4, nil), nil)) == "((4))");
            expect(print(h, h.cons(nil, nil)) == "(NIL)");
            expect(print(h, h.newv32({})) == "#<>");
            const auto vector =
                h.newv32(std::array{fixnum(-4), proper, h.newv08("x")});
            expect(print(h, vector) == "#<-4 (1 2 3) \"x\">");
            expect(
                print(h, h.cons(7, vector)) == "(7 . #<-4 (1 2 3) \"x\">)");
        };

        "function and macro diagnostics omit names' packages"_test = [] {
            heap h;
            auto name = symbol(h, "F", package(h, "OTHER"));
            auto f = h.make<tag::fun>({nil, nil, 17, name, 4});
            auto m = h.make<tag::mac>({nil, nil, 29, name, 3});
            expect(print(h, f) == "#'F");
            expect(print(h, m) == "#'F");
            h.set<tag::fun, field::sym>(f, nil);
            h.set<tag::mac, field::sym>(m, nil);
            expect(print(h, f) == "#<ANONYMOUS-FUNCTION>");
            expect(print(h, m) == "#<ANONYMOUS-MACRO>");
        };

        "continuation and run field names follow their schemas"_test = [] {
            heap h;
            auto ktx = h.make<tag::ktx>({top, nil, 7, 11, 13});
            expect(
                print(h, ktx)
                == "<%ktx hop=#<TOP> env=NIL fun=7 acc=11 arg=13>");
            auto run = h.make<tag::run>({17, nah, nil, 19, ktx, top});
            expect(
                print(h, run)
                == "<run exp=17 val=#<NAH> err=NIL env=19 way=<%ktx hop=#<TOP> env=NIL fun=7 acc=11 arg=13> meta=#<TOP>>");
        };

        "opaque diagnostics use local unsigned IDs and never follow pins"_test =
            [] {
                heap h;
                expect(print(h, immediate(tag::jet, 0)) == "#<jet 0>");
                expect(
                    print(h, immediate(tag::jet, max_immediate))
                    == "#<jet 134217727>");
                expect(
                    print(h, immediate(tag::chr, 0x1f642))
                    == "#<chr 128578>");
                // A freed pin is still printable without dereferencing it.
                const auto pin = h.make_pin(23);
                expect(print(h, pin) == "#<pin 1>");
                h.free_pin(pin);
                expect(print(h, pin) == "#<pin 1>");
                const auto ext = h.make<tag::ext>({0xffffffffu, 41});
                expect(print(h, ext) == "#<ext 4294967295>");
            };

        "cycles through list heads, tails, vectors, and machine rows"_test = [] {
            heap h;
            auto a = h.cons(1, nil);
            auto b = h.cons(2, a);
            h.set<tag::duo, field::cdr>(a, b);
            expect(print(h, a) == "(1 2 . #<CYCLE>)");
            h.set<tag::duo, field::cdr>(a, nil);
            h.set<tag::duo, field::car>(a, a);
            expect(print(h, a) == "(#<CYCLE>)");
            h.set<tag::duo, field::car>(a, 1);
            h.set<tag::duo, field::cdr>(a, b);
            h.put<tag::duo>(b, {a, nil});
            expect(print(h, a) == "(1 #<CYCLE>)");
            auto vector = h.filledv32(2, nil);
            h.v32set(vector, 0, vector);
            h.v32set(vector, 1, h.cons(7, vector));
            expect(print(h, vector) == "#<#<CYCLE> (7 . #<CYCLE>)>");
            auto ktx = h.make<tag::ktx>({top, nil, 3, nil, nil});
            auto run = h.make<tag::run>({5, nah, nil, nil, ktx, top});
            h.set<tag::ktx, field::hop>(ktx, run);
            expect(
                print(h, run)
                == "<run exp=5 val=#<NAH> err=NIL env=NIL way=<%ktx hop=#<CYCLE> env=NIL fun=3 acc=NIL arg=NIL> meta=#<TOP>>");
        };

        "acyclic sharing is not mistaken for recursion"_test = [] {
            heap h;
            auto tail = h.cons(2, h.cons(3, nil));
            auto both = h.cons(tail, tail);
            expect(print(h, both) == "((2 3) 2 3)");
            auto shared = h.newv32(std::array{tail, tail});
            expect(print(h, shared) == "#<(2 3) (2 3)>");
            auto ktx = h.make<tag::ktx>({top, tail, 7, tail, shared});
            expect(
                print(h, ktx)
                == "<%ktx hop=#<TOP> env=(2 3) fun=7 acc=(2 3) arg=#<(2 3) (2 3)>>");
        };

        "printing preserves guest data, era, pins, and collection"_test =
            [] {
                heap h;
                auto str = h.newv08("a\nb");
                auto list = h.cons(str, nil);
                auto vec = h.newv32(std::array{list, list});
                auto pin = h.make_pin(vec);
                root held{h, vec};
                const auto original_list = h.read<tag::duo>(list);
                const auto original_vector = h.read<tag::v32>(vec);
                const auto words = h.word_count();
                const auto bytes = h.byte_count();
                const auto era = h.era();
                const auto before = print(h, held.get());
                expect(before == "#<(\"a\\nb\") (\"a\\nb\")>");
                expect(h.read<tag::duo>(list) == original_list);
                expect(h.read<tag::v32>(vec) == original_vector);
                expect(h.v32slice(vec)[0] == list);
                expect(h.v32slice(vec)[1] == list);
                expect(h.v08slice(str) == "a\nb");
                expect(h.word_count() == words && h.byte_count() == bytes);
                expect(h.era() == era && held.get() == vec);
                expect(h.pinned(pin) == vec);
                h.collect();
                expect(print(h, held.get()) == before);
                expect(h.pinned(pin) == held.get());
                h.free_pin(pin);
            };

        "deep mixed nesting uses host storage rather than recursion"_test =
            [] {
                heap h;
                constexpr auto depth = 30000;
                auto x = fixnum(-9);
                std::string prefix, suffix;
                for (int i = 0; i < depth; ++i) {
                    // Each pair opens with a list then a vector.
                    x = h.cons(h.newv32(std::array{x}), nil);
                    prefix += "(#<";
                    suffix += ">)";
                }
                expect(print(h, x) == prefix + "-9" + suffix);
            };

        "large flat lists and vectors retain every element"_test = [] {
            heap h;
            constexpr auto length = 60000;
            auto list = nil;
            std::vector<word> words;
            std::string body;
            for (int i = 0; i < length; ++i) {
                words.push_back(fixnum(i));
                if (i != 0)
                    body += ' ';
                body += std::to_string(i);
                list = h.cons(fixnum(length - i - 1), list);
            }
            expect(print(h, list) == '(' + body + ')');
            expect(print(h, h.newv32(words)) == "#<" + body + '>');
        };
    }};

} // namespace
} // namespace wisp::test
