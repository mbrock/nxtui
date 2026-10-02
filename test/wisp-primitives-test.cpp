// SPDX-License-Identifier: AGPL-3.0-or-later
// Data primitive semantics from mbrock/wisp's core/jets-fun.zig.
#include <wisp/eval.hpp>

#include "test.hpp"

#include <initializer_list>

namespace wisp::test {
namespace {

using namespace boost::ut;

struct primitives
{
    heap h;
    evaluator vm{h};
    root running{h};

    word s(std::string_view name)
    {
        return vm.intern(name);
    }

    word l(std::initializer_list<word> xs)
    {
        word result = nil;
        for (auto i = xs.size(); i != 0; --i)
            result = h.cons(xs.begin()[i - 1], result);
        return result;
    }

    word f(std::string_view name, std::initializer_list<word> xs = {})
    {
        return h.cons(s(name), l(xs));
    }

    word q(word x)
    {
        return f("QUOTE", {x});
    }

    word call_form(std::string_view name, std::initializer_list<word> xs)
    {
        auto args = nil;
        for (auto i = xs.size(); i != 0; --i)
            args = h.cons(q(xs.begin()[i - 1]), args);
        return h.cons(s(name), args);
    }

    evaluation execute(word form)
    {
        running.set(vm.start(form));
        for (unsigned i = 0; i < 2000; ++i) {
            h.collect();
            const auto state = vm.advance(running.get(), 1);
            if (state != evaluation::runnable)
                return state;
        }
        throw std::runtime_error(
            "Wisp primitive test exhausted its budget");
    }

    word eval(word form)
    {
        if (execute(form) != evaluation::done)
            throw std::runtime_error("Wisp primitive unexpectedly failed");
        return h.get<tag::run, field::val>(running.get());
    }

    word call(std::string_view name, std::initializer_list<word> xs = {})
    {
        return eval(call_form(name, xs));
    }

    word error(std::string_view name, std::initializer_list<word> xs)
    {
        if (execute(call_form(name, xs)) != evaluation::failed)
            throw std::runtime_error(
                "Wisp primitive unexpectedly succeeded");
        auto error = h.get<tag::run, field::err>(running.get());
        expect(h.v32slice(error)[0] == s("UNHANDLED-ERROR"));
        error = h.v32slice(error)[2];
        while (h.v32slice(error)[0] == s("BUILTIN-FAILURE"))
            error = h.v32slice(error)[2];
        return error;
    }

    void fails(
        std::string_view name,
        std::initializer_list<word> xs,
        std::string_view condition)
    {
        const auto failure = error(name, xs);
        expect(h.v32slice(failure)[0] == s(condition));
    }

    void words(word vector, std::initializer_list<word> expected)
    {
        expect(std::ranges::equal(h.v32slice(vector), expected));
    }
};

static suite primitive_tests{
    "WISP PRIMITIVES", [] {
        "type names distinguish NIL, T, macros and control jets"_test = [] {
            primitives m;
            const auto pin = m.h.make_pin(nil);
            root samples{
                m.h,
                m.h.newv32(
                    std::array{
                        nil,
                        t,
                        fixnum(-19),
                        immediate(tag::chr, 955),
                        m.h.cons(3, nil),
                        m.s("X"),
                        m.h.make<tag::fun>({nil, nil, 7, nil, 0}),
                        m.h.make<tag::mac>({nil, nil, 11, nil, 0}),
                        m.h.get<tag::sym, field::fun>(m.s("QUOTE")),
                        m.h.newv32({}),
                        m.h.newv08("str"),
                        m.vm.find_package("WISP"),
                        m.h.make<tag::ktx>({top, nil, nil, nil, nil}),
                        m.vm.start(17),
                        m.h.make<tag::ext>({42, nil}),
                        pin,
                        top})};
            constexpr std::array names{
                "NULL",
                "BOOLEAN",
                "INTEGER",
                "CHARACTER",
                "CONS",
                "SYMBOL",
                "FUNCTION",
                "MACRO",
                "FUNCTION",
                "VECTOR",
                "STRING",
                "PACKAGE",
                "CONTINUATION",
                "EVALUATOR",
                "EXTERNAL",
                "PIN",
                "CONTINUATION"};
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto result =
                    m.call("TYPE-OF", {m.h.v32slice(samples.get())[i]});
                expect(result == m.s(names[i]));
                expect(result != m.vm.keyword(names[i]));
            }
            m.h.free_pin(pin);
            for (auto name : {"QUOTE", "+"}) {
                const auto jet = m.h.get<tag::sym, field::fun>(m.s(name));
                expect(m.call("JET?", {jet}) == t);
                expect(
                    m.call("JET-CTL?", {jet})
                    == (name == std::string_view{"QUOTE"} ? t : nil));
            }
            expect(m.call("JET?", {m.h.v32slice(samples.get())[6]}) == nil);
            expect(m.call("JET-CTL?", {nil}) == nil);
            // NAH is evaluator control state, not an evaluable literal.
            // APPLY can pass it as data without first returning it from
            // QUOTE through the val register.
            m.fails(
                "APPLY",
                {m.h.get<tag::sym, field::fun>(m.s("TYPE-OF")), m.l({nah})},
                "INVALID-VALUE");
        };

        "PROGNIFY preserves singleton and body identity, rejecting malformed lists"_test =
            [] {
                primitives m;
                expect(m.call("PROGNIFY", {nil}) == nil);
                root one{m.h, m.f("+", {7, 11})};
                auto result = m.call("PROGNIFY", {m.l({one.get()})});
                expect(result == one.get());
                root body{m.h, m.l({one.get(), 29})};
                result = m.call("PROGNIFY", {body.get()});
                expect(
                    (m.h.read<tag::duo>(result)
                     == row<tag::duo>{m.s("DO"), body.get()}));
                expect(m.eval(result) == 29u);
                m.fails("PROGNIFY", {m.h.cons(3, 4)}, "TYPE-MISMATCH");
                root cycle{m.h, m.l({3, 5})};
                m.h.set<tag::duo, field::cdr>(
                    m.h.get<tag::duo, field::cdr>(cycle.get()),
                    cycle.get());
                m.fails("PROGNIFY", {cycle.get()}, "CYCLIC-LIST");
                m.fails("VECTOR-FROM-LIST", {cycle.get()}, "CYCLIC-LIST");
            };

        "MACROEXPAND-1 calls the macro once without evaluating its result"_test =
            [] {
                primitives m;
                auto x = m.s("X"), arg = m.s("ARG");
                m.eval(
                    m.f("LET",
                        {m.l({m.l({x, 11})}),
                         m.f("SET-SYMBOL-FUNCTION!",
                             {m.q(m.s("M")),
                              m.f("%MACRO-FN",
                                  {m.l({arg}),
                                   m.f("LIST",
                                       {m.q(m.s("NEXT")), arg, x})})})}));
                m.eval(m.f(
                    "SET-SYMBOL-FUNCTION!",
                    {m.q(m.s("NEXT")),
                     m.f("%MACRO-FN", {m.l({m.s("A"), m.s("B")}), 37})}));
                x = m.s("X");
                const auto result = m.eval(
                    m.f("LET",
                        {m.l({m.l({x, 99})}),
                         m.f("VECTOR",
                             {m.f("MACROEXPAND-1",
                                  {m.q(m.f("M", {m.s("UNBOUND")}))}),
                              x})}));
                auto expansion = m.h.v32slice(result)[0];
                expect(m.h.v32slice(result)[1] == 99u);
                expect(
                    (m.h.get<tag::duo, field::car>(expansion)
                     == m.s("NEXT")));
                expansion = m.h.get<tag::duo, field::cdr>(expansion);
                expect(
                    (m.h.get<tag::duo, field::car>(expansion)
                     == m.s("UNBOUND")));
                expansion = m.h.get<tag::duo, field::cdr>(expansion);
                expect(
                    (m.h.read<tag::duo>(expansion)
                     == row<tag::duo>{11, nil}));
                expect(
                    m.call(
                        "FUNCTION-CALL-COUNT",
                        {m.h.get<tag::sym, field::fun>(m.s("M"))})
                    == 1u);
                expect(
                    m.call(
                        "FUNCTION-CALL-COUNT",
                        {m.h.get<tag::sym, field::fun>(m.s("NEXT"))})
                    == 0u);
                root unchanged{m.h, m.h.cons(m.s("+"), fixnum(-1))};
                const auto same =
                    m.call("MACROEXPAND-1", {unchanged.get()});
                expect(same == unchanged.get());
                m.fails(
                    "MACROEXPAND-1",
                    {m.h.cons(m.s("M"), 7)},
                    "TYPE-MISMATCH");
            };

        "closure code and names mutate without replacing captured scope"_test =
            [] {
                primitives m;
                auto x = m.s("X");
                root fun{
                    m.h,
                    m.eval(m.f(
                        "LET",
                        {m.l({m.l({x, 17})}), m.f("%FN", {nil, nil, x})}))};
                auto result =
                    m.call("SET-SYMBOL-FUNCTION!", {m.s("F"), fun.get()});
                expect(result == fun.get());
                result = m.call("FUNCTION-NAME", {fun.get()});
                expect(result == m.s("F"));
                expect(m.eval(m.f("F")) == 17u);
                root code{m.h, m.f("+", {m.s("X"), 5})};
                result = m.call("SET-CODE!", {fun.get(), code.get()});
                expect(result == fun.get());
                result = m.call("CODE", {fun.get()});
                expect(result == code.get());
                expect(m.eval(m.f("F")) == 22u);
                result =
                    m.call("SET-FUNCTION-NAME!", {fun.get(), m.s("ALIAS")});
                expect(result == fun.get());
                result = m.call("FUNCTION-NAME", {fun.get()});
                expect(result == m.s("ALIAS"));
                expect(m.call("FUNCTION-CALL-COUNT", {fun.get()}) == 2u);
                root mac{m.h, m.eval(m.f("%MACRO-FN", {nil, 31}))};
                result = m.call("SET-CODE!", {mac.get(), 43});
                expect(result == mac.get());
                expect(m.call("CALL", {mac.get()}) == 43u);
                expect(m.call("FUNCTION-CALL-COUNT", {mac.get()}) == 1u);
                const auto jet = m.h.get<tag::sym, field::fun>(m.s("+"));
                expect(m.call("CODE", {jet}) == nil);
                expect(m.call("FUNCTION-CALL-COUNT", {jet}) == 0u);
                result = m.call("FUNCTION-NAME", {jet});
                expect(result == m.s("+"));
                expect(m.call("FUNCTION-NAME", {19}) == nil);
                m.fails("CODE", {19}, "PROGRAM-ERROR");
                m.fails("SET-CODE!", {jet, 7}, "PROGRAM-ERROR");
                m.fails("SET-FUNCTION-NAME!", {nil, nil}, "PROGRAM-ERROR");
                m.fails(
                    "SET-FUNCTION-NAME!", {fun.get(), 19}, "TYPE-MISMATCH");
                result = m.call("FUNCTION-NAME", {fun.get()});
                expect(result == m.s("ALIAS"));
                m.call("SET-FUNCTION-NAME!", {fun.get(), nil});
                result = m.call("PRINT-TO-STRING", {fun.get()});
                expect(m.h.v08slice(result) == "#<ANONYMOUS-FUNCTION>");
                expect(
                    m.execute(m.f("%FN", {19, nil, 7}))
                    == evaluation::failed);
            };

        "pair mutation returns the receiver and preserves cycles across GC"_test =
            [] {
                primitives m;
                root pair{m.h, m.h.cons(7, 11)};
                root vector{
                    m.h, m.h.newv32(std::array{pair.get(), fixnum(-13)})};
                auto result =
                    m.call("SET-HEAD!", {pair.get(), vector.get()});
                expect(result == pair.get());
                result = m.call("SET-TAIL!", {pair.get(), pair.get()});
                expect(result == pair.get());
                m.h.collect();
                expect(
                    (m.h.read<tag::duo>(pair.get())
                     == row<tag::duo>{vector.get(), pair.get()}));
                m.words(vector.get(), {pair.get(), fixnum(-13)});
                m.fails("SET-HEAD!", {nil, 1}, "TYPE-MISMATCH");
                m.fails("SET-TAIL!", {vector.get(), nil}, "TYPE-MISMATCH");
            };

        "vectors copy append payloads but retain element identity"_test =
            [] {
                primitives m;
                root object{m.h, m.h.cons(19, nil)};
                root a{
                    m.h, m.call("VECTOR", {object.get(), fixnum(-7), t})};
                root b{m.h, m.call("VECTOR-FROM-LIST", {m.l({nil, 23})})};
                root joined{
                    m.h,
                    m.call("VECTOR-APPEND", {a.get(), b.get(), a.get()})};
                m.words(
                    joined.get(),
                    {object.get(),
                     fixnum(-7),
                     t,
                     nil,
                     23,
                     object.get(),
                     fixnum(-7),
                     t});
                expect(m.call("VECTOR-SET!", {a.get(), 1, 41}) == 41u);
                expect(m.call("VECTOR-GET", {a.get(), 1}) == 41u);
                expect(
                    m.call("VECTOR-GET", {joined.get(), 1}) == fixnum(-7));
                auto result = m.call("VECTOR-GET", {joined.get(), 5});
                expect(result == object.get());
                expect(m.call("VECTOR-LENGTH", {joined.get()}) == 8u);
                expect(m.h.v32slice(m.call("VECTOR")).empty());
                expect(m.h.v32slice(m.call("VECTOR-APPEND")).empty());
                expect(m.h.v32slice(m.call("VECTOR-FROM-LIST", {nil}))
                           .empty());
                root large{m.h, m.h.filledv32(4097, 13)};
                m.h.v32set(large.get(), 0, 17);
                m.h.v32set(large.get(), 4096, 31);
                root grown{
                    m.h,
                    m.call(
                        "VECTOR-APPEND",
                        {large.get(), a.get(), large.get()})};
                expect(m.call("VECTOR-GET", {grown.get(), 4096}) == 31u);
                result = m.call("VECTOR-GET", {grown.get(), 4097});
                expect(result == object.get());
                expect(m.call("VECTOR-GET", {grown.get(), 4100}) == 17u);
                expect(m.call("VECTOR-GET", {grown.get(), 8196}) == 31u);
            };

        "vector indices are signed and rejected mutations leave the payload intact"_test =
            [] {
                primitives m;
                root vec{m.h, m.call("VECTOR", {11, 29})};
                for (auto idx :
                     {fixnum(-1),
                      fixnum(min_fixnum),
                      word{2},
                      fixnum(max_fixnum),
                      nil}) {
                    m.fails(
                        "VECTOR-GET", {vec.get(), idx}, "TYPE-MISMATCH");
                    m.fails(
                        "VECTOR-SET!",
                        {vec.get(), idx, 47},
                        "TYPE-MISMATCH");
                    m.words(vec.get(), {11, 29});
                }
                expect(m.call("VECTOR-GET", {vec.get(), 0}) == 11u);
                expect(m.call("VECTOR-GET", {vec.get(), 1}) == 29u);
                m.fails(
                    "VECTOR-GET", {m.h.newv08("ab"), 0}, "TYPE-MISMATCH");
                m.fails("VECTOR-LENGTH", {nil}, "TYPE-MISMATCH");
                m.fails("VECTOR-APPEND", {vec.get(), nil}, "TYPE-MISMATCH");
                m.fails(
                    "VECTOR-FROM-LIST", {m.h.cons(1, 2)}, "TYPE-MISMATCH");
            };

        "strings use byte offsets, ASCII uppercase and fresh slice storage"_test =
            [] {
                primitives m;
                const std::string text{"a\0\xc3\xa9Zab", 7};
                root str{m.h, m.h.newv08(text)};
                expect(m.call("STRING-LENGTH", {str.get()}) == 7u);
                expect(m.call("BYTE-SIZE", {str.get()}) == 7u);
                expect(
                    m.call("STRING-SEARCH", {str.get(), m.h.newv08("Za")})
                    == 4u);
                expect(
                    m.call(
                        "STRING-SEARCH", {str.get(), m.h.newv08("absent")})
                    == nil);
                expect(
                    m.call("STRING-SEARCH", {str.get(), m.h.newv08("")})
                    == 0u);
                auto result = m.call("STRING-TO-UPPERCASE", {str.get()});
                expect(
                    m.h.v08slice(result)
                    == std::string_view{"A\0\xc3\xa9ZAB", 7});
                expect(m.h.v08slice(str.get()) == text);
                result = m.call("STRING-SLICE", {str.get(), 1, 4});
                expect(
                    m.h.v08slice(result)
                    == std::string_view{"\0\xc3\xa9", 3});
                result = m.call("STRING-SLICE", {str.get(), 0, 7});
                expect(result != str.get());
                expect(m.h.v08slice(result) == text);
                expect(m.call("STRING-EQUAL?", {str.get(), result}) == t);
                expect(
                    m.call("STRING-EQUAL?", {str.get(), m.h.newv08("a")})
                    == nil);
                result = m.call("STRING-SLICE", {str.get(), 7, 7});
                expect(m.h.v08slice(result).empty());
                expect(m.h.v08slice(m.call("STRING-APPEND")).empty());
                root big{m.h, m.h.newv08(std::string(8193, 'q'))};
                result = m.call(
                    "STRING-APPEND", {str.get(), big.get(), str.get()});
                expect(
                    m.h.v08slice(result)
                    == text + std::string(8193, 'q') + text);
                // Repeated slices grow the same byte pool that supplies
                // input.
                for (int i = 0; i < 8; ++i) {
                    result = m.call("STRING-SLICE", {big.get(), 1, 8193});
                    expect(m.h.v08slice(result) == std::string(8192, 'q'));
                }
            };

        "string bounds reject reversed, negative, oversized and noninteger indices"_test =
            [] {
                primitives m;
                root str{m.h, m.h.newv08("abcde")};
                for (auto bounds :
                     {std::array{fixnum(-1), word{2}},
                      std::array{word{3}, word{2}},
                      std::array{word{0}, word{6}},
                      std::array{word{6}, word{6}},
                      std::array{word{0}, fixnum(min_fixnum)}}) {
                    const auto error = m.error(
                        "STRING-SLICE", {str.get(), bounds[0], bounds[1]});
                    m.words(
                        error,
                        {m.s("BOUNDS-ERROR"),
                         str.get(),
                         bounds[0],
                         bounds[1],
                         5});
                }
                m.fails(
                    "STRING-SLICE", {str.get(), nil, 2}, "TYPE-MISMATCH");
                m.fails("STRING-SEARCH", {str.get(), nil}, "TYPE-MISMATCH");
                m.fails("STRING-EQUAL?", {nil, str.get()}, "TYPE-MISMATCH");
                m.fails("STRING-APPEND", {str.get(), nil}, "TYPE-MISMATCH");
                m.fails("STRING-LENGTH", {nil}, "TYPE-MISMATCH");
                m.fails("STRING-TO-UPPERCASE", {nil}, "TYPE-MISMATCH");
            };

        "string readers distinguish NIL from EOF and expose malformed input as conditions"_test =
            [] {
                primitives m;
                expect(
                    m.call("READ-FROM-STRING", {m.h.newv08("nil 42")})
                    == nil);
                expect(
                    m.call("READ-FROM-STRING", {m.h.newv08("42 )")})
                    == 42u);
                expect(
                    m.call("READ-MANY-FROM-STRING", {m.h.newv08("; EOF")})
                    == nil);
                m.fails(
                    "READ-FROM-STRING", {m.h.newv08("")}, "END-OF-FILE");
                m.fails("READ-FROM-STRING", {nil}, "TYPE-MISMATCH");
                m.fails(
                    "READ-MANY-FROM-STRING",
                    {m.h.newv08("42 )")},
                    "READ-ERROR");
                m.fails(
                    "READ-FROM-STRING",
                    {m.h.newv08("\"\\t\"")},
                    "READ-ERROR");
                const auto source = std::string("nil \"")
                                    + std::string(8193, 'x') + "\" 73";
                root forms{
                    m.h,
                    m.call("READ-MANY-FROM-STRING", {m.h.newv08(source)})};
                expect((m.h.get<tag::duo, field::car>(forms.get()) == nil));
                auto tail = m.h.get<tag::duo, field::cdr>(forms.get());
                expect(
                    m.h.v08slice(m.h.get<tag::duo, field::car>(tail))
                    == std::string(8193, 'x'));
                tail = m.h.get<tag::duo, field::cdr>(tail);
                expect(
                    (m.h.read<tag::duo>(tail) == row<tag::duo>{73, nil}));
            };

        "string streams retain byte cursors through UTF-8, pool growth, EOF and errors"_test =
            [] {
                primitives m;
                root stream{
                    m.h,
                    m.h.newv32(
                        std::array{
                            m.s("STRING-INPUT-STREAM"),
                            word{0},
                            m.h.newv08("é nil 29 ;EOF")})};
                auto result =
                    m.call("READ-FROM-STRING-STREAM!", {stream.get()});
                expect(
                    (m.h.read<tag::duo>(result)
                     == row<tag::duo>{m.s("é"), nil}));
                expect(m.h.v32slice(stream.get())[1] == 2u);
                result = m.call("READ-FROM-STRING-STREAM!", {stream.get()});
                expect((
                    m.h.read<tag::duo>(result) == row<tag::duo>{nil, nil}));
                expect(m.h.v32slice(stream.get())[1] == 6u);
                result = m.call("READ-FROM-STRING-STREAM!", {stream.get()});
                expect(
                    (m.h.read<tag::duo>(result) == row<tag::duo>{29, nil}));
                expect(m.h.v32slice(stream.get())[1] == 9u);
                for (int i = 0; i < 2; ++i) {
                    expect(
                        m.call("READ-FROM-STRING-STREAM!", {stream.get()})
                        == nil);
                    expect(m.h.v32slice(stream.get())[1] == 14u);
                }
                for (auto offset : {fixnum(-1), word{15}, nil}) {
                    m.h.v32set(stream.get(), 1, offset);
                    m.fails(
                        "READ-FROM-STRING-STREAM!",
                        {stream.get()},
                        offset == nil ? "TYPE-MISMATCH" : "BOUNDS-ERROR");
                    expect(m.h.v32slice(stream.get())[1] == offset);
                }
                m.h.v32set(stream.get(), 1, 0);
                m.h.v32set(stream.get(), 2, m.h.newv08("(unfinished"));
                m.fails(
                    "READ-FROM-STRING-STREAM!",
                    {stream.get()},
                    "READ-ERROR");
                expect(m.h.v32slice(stream.get())[1] == 0u);
                const auto source =
                    std::string("\"") + std::string(8193, 'q') + "\" 11";
                m.h.v32set(stream.get(), 2, m.h.newv08(source));
                result = m.call("READ-FROM-STRING-STREAM!", {stream.get()});
                expect(
                    m.h.v08slice(m.h.get<tag::duo, field::car>(result))
                    == std::string(8193, 'q'));
                expect(m.h.v32slice(stream.get())[1] == 8195u);
                result = m.call("READ-FROM-STRING-STREAM!", {stream.get()});
                expect(
                    (m.h.read<tag::duo>(result) == row<tag::duo>{11, nil}));
                m.fails("READ-FROM-STRING-STREAM!", {nil}, "TYPE-MISMATCH");
                m.fails(
                    "READ-FROM-STRING-STREAM!",
                    {m.h.newv32({})},
                    "INVALID-STRING-INPUT-STREAM");
                m.h.v32set(stream.get(), 0, m.s("OTHER"));
                m.fails(
                    "READ-FROM-STRING-STREAM!",
                    {stream.get()},
                    "INVALID-STRING-INPUT-STREAM");
            };

        "division floors at each step and MOD accepts only positive divisors"_test =
            [] {
                primitives m;
                for (auto row :
                     {std::array{-7, 3, -3},
                      std::array{7, -3, -3},
                      std::array{-7, -3, 2},
                      std::array{-6, 3, -2},
                      std::array{7, 3, 2},
                      std::array{min_fixnum, 1, min_fixnum}})
                    expect(
                        m.call("/", {fixnum(row[0]), fixnum(row[1])})
                        == fixnum(row[2]));
                expect(m.call("/", {fixnum(-19), 3, 2}) == fixnum(-4));
                expect(m.call("/", {2}) == 0u);
                expect(m.call("/", {fixnum(-2)}) == fixnum(-1));
                expect(m.call("MOD", {fixnum(-7), 3}) == 2u);
                expect(m.call("MOD", {7, 3}) == 1u);
                expect(m.call("MOD", {fixnum(min_fixnum), 3}) == 2u);
                expect(m.call("MOD", {fixnum(-6), 3}) == 0u);
                auto error = m.error("/", {fixnum(-7), 3, 0});
                m.words(error, {m.s("BAD-FIXNUM-DIVISION"), fixnum(-3), 0});
                error = m.error("/", {fixnum(min_fixnum), fixnum(-1)});
                m.words(
                    error,
                    {m.s("BAD-FIXNUM-DIVISION"),
                     fixnum(min_fixnum),
                     fixnum(-1)});
                m.fails("/", {}, "PROGRAM-ERROR");
                m.fails("/", {0}, "BAD-FIXNUM-DIVISION");
                m.fails("/", {1, nil}, "TYPE-MISMATCH");
                m.fails("MOD", {7, 0}, "BAD-MODULO");
                m.fails("MOD", {7, fixnum(-3)}, "BAD-MODULO");
                m.fails("MOD", {nil, 3}, "TYPE-MISMATCH");
            };

        "package queries and qualified interning retain exact symbol identities"_test =
            [] {
                primitives m;
                root base{m.h, m.vm.find_package("WISP")};
                root keywords{m.h, m.vm.find_package("KEYWORD")};
                expect(m.vm.find_package("wisp") == nil);
                expect(m.vm.intern("NIL", base.get()) == nil);
                expect(m.vm.intern("T", base.get()) == t);
                root keyword{m.h, m.vm.intern("NIL", keywords.get())};
                expect(keyword.get() != nil);
                expect(
                    (m.h.get<tag::sym, field::val>(keyword.get()) == nah));
                auto result = m.eval(keyword.get());
                expect(result == keyword.get());
                result = m.call("FIND-PACKAGE", {m.h.newv08("WISP")});
                expect(result == base.get());
                expect(m.call("FIND-PACKAGE", {m.h.newv08("wisp")}) == nil);
                root lower{
                    m.h,
                    m.call("INTERN", {m.h.newv08("mixed"), base.get()})};
                expect(lower.get() != m.s("MIXED"));
                result = m.call("SYMBOL-NAME", {lower.get()});
                expect(m.h.v08slice(result) == "mixed");
                result = m.call("SYMBOL-PACKAGE", {lower.get()});
                expect(result == base.get());
                for (auto special : {nil, t}) {
                    expect(m.call("SYMBOL?", {special}) == t);
                    result = m.call("SYMBOL-PACKAGE", {special});
                    expect(result == base.get());
                    root name{m.h, m.call("SYMBOL-NAME", {special})};
                    expect(
                        m.h.v08slice(name.get())
                        == (special == nil ? "NIL" : "T"));
                    result = m.call("SYMBOL-NAME", {special});
                    expect(result == name.get());
                    expect(m.call("SYMBOL-FUNCTION", {special}) == nil);
                }
                expect(m.call("SYMBOL?", {keyword.get()}) == t);
                expect(m.call("SYMBOL?", {0}) == nil);
                expect(m.call("SYMBOL?", {top}) == nil);
                result = m.call("PACKAGE-SYMBOLS", {base.get()});
                expect(
                    result != (m.h.get<tag::pkg, field::sym>(base.get())));
                auto found = false;
                for (auto cur = result; cur != nil;
                     cur = m.h.get<tag::duo, field::cdr>(cur))
                    found |=
                        m.h.get<tag::duo, field::car>(cur) == lower.get();
                expect(found);
                m.h.set<tag::duo, field::car>(result, 19);
                m.h.set<tag::duo, field::cdr>(result, result);
                expect(m.vm.intern("mixed", base.get()) == lower.get());
                expect(
                    tag_of(m.vm.intern("NEW-SYMBOL", base.get()))
                    == tag::sym);
                result = m.call("PACKAGE-NAME", {keywords.get()});
                expect(m.h.v08slice(result) == "KEYWORD");
                expect(m.call("PACKAGE-USES", {base.get()}) == nil);
                result = m.call("PACKAGES");
                expect(
                    (m.h.get<tag::duo, field::car>(result)
                     == m.vm.find_package("KEY")));
                result = m.h.get<tag::duo, field::cdr>(result);
                expect(
                    (m.h.get<tag::duo, field::car>(result)
                     == keywords.get()));
                result = m.h.get<tag::duo, field::cdr>(result);
                expect(
                    (m.h.read<tag::duo>(result)
                     == row<tag::duo>{base.get(), nil}));
                m.fails("INTERN", {m.h.newv08("X"), nil}, "TYPE-MISMATCH");
                m.fails("FIND-PACKAGE", {7}, "TYPE-MISMATCH");
                m.fails("PACKAGE-SYMBOLS", {nil}, "TYPE-MISMATCH");
                m.fails("SYMBOL-NAME", {7}, "TYPE-MISMATCH");
            };

        "primitive DEFPACKAGE keeps raw names and ordered imports, rejecting partial declarations"_test =
            [] {
                primitives m;
                root left{m.h, m.call("%DEFPACKAGE", {m.h.newv08("LEFT")})};
                root right{
                    m.h, m.call("%DEFPACKAGE", {m.h.newv08("RIGHT")})};
                root left_name{m.h, m.vm.intern("SHARED", left.get())};
                root right_name{m.h, m.vm.intern("SHARED", right.get())};
                root imports{m.h, m.l({right.get(), left.get()})};
                root app{
                    m.h,
                    m.eval(m.f("DEFPACKAGE", {m.s("APP"), imports.get()}))};
                expect(app.get() == m.vm.find_package("APP"));
                expect(
                    m.h.get<tag::pkg, field::use>(app.get())
                    == imports.get());
                // APP is unbound: this control builtin must not evaluate
                // its name or import list, and the first import wins.
                expect(
                    m.vm.intern("SHARED", app.get()) == right_name.get());
                expect(left_name.get() != right_name.get());
                const auto selected =
                    m.eval(m.f("IN-PACKAGE", {m.s("APP")}));
                expect(selected == app.get());
                expect(m.vm.current_package() == app.get());
                expect(m.vm.intern("LOCAL", app.get()) != m.s("LOCAL"));

                for (bool duplicate : {false, true}) {
                    const auto form =
                        m.f("DEFPACKAGE",
                            {m.s(duplicate ? "APP" : "REJECTED"),
                             duplicate ? nil : m.l({left.get(), 17})});
                    expect(m.execute(form) == evaluation::failed);
                    auto error = m.h.v32slice(
                        m.h.get<tag::run, field::err>(m.running.get()))[2];
                    while (m.h.v32slice(error)[0] == m.s("BUILTIN-FAILURE"))
                        error = m.h.v32slice(error)[2];
                    expect(
                        m.h.v32slice(error)[0]
                        == m.s(
                            duplicate ? "PACKAGE-EXISTS"
                                      : "TYPE-MISMATCH"));
                    expect(m.vm.find_package("REJECTED") == nil);
                    expect(m.vm.find_package("APP") == app.get());
                    expect(
                        m.h.get<tag::pkg, field::use>(app.get())
                        == imports.get());
                }
            };

        "fresh keys skip interned serials and self-evaluate across turns"_test =
            [] {
                primitives m;
                root reserved{
                    m.h,
                    m.call(
                        "INTERN",
                        {m.h.newv08("~20220101.BYYYYYYYYY"),
                         m.vm.find_package("KEY")})};
                root first{m.h, m.call("FRESH-SYMBOL!")};
                root second{m.h, m.call("GENKEY!")};
                expect(
                    first.get() != second.get()
                    && first.get() != reserved.get());
                auto result = m.eval(first.get());
                expect(result == first.get());
                expect(m.call("KEY?", {first.get()}) == t);
                expect(m.call("KEY?", {reserved.get()}) == t);
                expect(
                    m.call("KEY?", {m.vm.keyword("~20220101.BYYYYYYYYY")})
                    == nil);
                expect(m.call("KEY?", {nil}) == nil);
                result = m.call("SYMBOL-PACKAGE", {second.get()});
                expect(result == m.vm.find_package("KEY"));
                result = m.call("SYMBOL-NAME", {first.get()});
                expect(m.h.v08slice(result) == "~20220101.NYYYYYYYYY");
                result = m.call("SYMBOL-NAME", {second.get()});
                expect(m.h.v08slice(result) == "~20220101.DYYYYYYYYY");
                expect(
                    (m.h.get<tag::sym, field::val>(reserved.get()) == nah));
                root latest{m.h};
                for (int serial = 4; serial <= 32; ++serial)
                    latest.set(m.call("GENKEY!"));
                result = m.call("SYMBOL-NAME", {latest.get()});
                expect(m.h.v08slice(result) == "~20220101.YBYYYYYYYY");
            };

        "pin lifetime is independent of handle reachability and release is idempotent"_test =
            [] {
                primitives m;
                const auto pin = m.call(
                    "MAKE-PINNED-VALUE",
                    {m.h.newv32(std::array{word{17}, word{31}})});
                m.running.set(nil);
                m.h.collect();
                expect(m.h.table<tag::v32>().size() == 1u);
                m.words(m.h.pinned(pin), {17, 31});
                expect(m.call("RELEASE-PINNED-VALUE!", {pin}) == nil);
                m.running.set(nil);
                m.h.collect();
                expect(m.h.table<tag::v32>().size() == 0u);
                expect(m.call("RELEASE-PINNED-VALUE!", {pin}) == nil);
                m.fails("RELEASE-PINNED-VALUE!", {1}, "TYPE-MISMATCH");
            };

        "RUN captures lexical state and exposes expression versus value"_test =
            [] {
                primitives m;
                const auto x = m.s("X");
                root run{
                    m.h,
                    m.eval(
                        m.f("LET",
                            {m.l({m.l({x, 19})}),
                             m.f("RUN", {m.q(m.f("+", {x, 23}))})}))};
                auto result = m.call("RUN-EXP", {run.get()});
                expect(
                    (m.h.get<tag::duo, field::car>(result) == m.s("EXP")));
                expect(
                    (m.h.get<tag::duo, field::cdr>(result)
                     == m.h.get<tag::run, field::exp>(run.get())));
                expect(m.call("RUN-WAY", {run.get()}) == top);
                expect(m.call("RUN-ERR", {run.get()}) == nil);
                for (int i = 0;
                     i < 30
                     && m.vm.status(run.get()) == evaluation::runnable;
                     ++i) {
                    m.h.collect();
                    m.vm.step(run.get());
                }
                expect(m.vm.status(run.get()) == evaluation::done);
                result = m.call("RUN-EXP", {run.get()});
                expect(
                    (m.h.read<tag::duo>(result)
                     == row<tag::duo>{m.s("VAL"), 42}));
                expect(m.call("RUN-VAL", {run.get()}) == 42u);
                m.fails("RUN-WAY", {nil}, "TYPE-MISMATCH");
                m.fails("RUN-EXP", {nil}, "TYPE-MISMATCH");
            };
    }};

} // namespace
} // namespace wisp::test
