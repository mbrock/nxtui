#include <wisp/eval.hpp>

#include "test.hpp"

#include <initializer_list>

namespace wisp::test {
namespace {

using namespace boost::ut;

// Build forms directly: these tests exercise evaluation, not a second
// reader.
struct language
{
    heap h;
    evaluator vm{h};
    root run{h};

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

    word fn(word pars, word body)
    {
        return f("%FN", {nil, pars, body});
    }

    evaluation execute(word exp, bool collect = false)
    {
        run.set(vm.start(exp));
        for (unsigned i = 0; i < 10000; ++i) {
            if (collect)
                h.collect();
            const auto state = vm.advance(run.get(), 1);
            if (state != evaluation::runnable)
                return state;
        }
        throw std::runtime_error("Wisp test exhausted its step limit");
    }

    word eval(word exp, bool collect = false)
    {
        if (execute(exp, collect) != evaluation::done)
            throw std::runtime_error("Wisp test unexpectedly failed");
        return h.get<tag::run, field::val>(run.get());
    }

    word error(word exp)
    {
        expect(execute(exp, true) == evaluation::failed);
        const auto before = h.read<tag::run>(run.get());
        expect(vm.advance(run.get(), 100) == evaluation::failed);
        expect(h.read<tag::run>(run.get()) == before);
        const auto condition =
            h.v32slice(h.get<tag::run, field::err>(run.get()));
        expect(condition.size() == 3u);
        expect(condition[0] == s("UNHANDLED-ERROR"));
        expect(condition[1] == s("ERROR"));
        return condition[2];
    }

    word cause(word error)
    {
        while (h.v32slice(error)[0] == s("BUILTIN-FAILURE"))
            error = h.v32slice(error)[2];
        return error;
    }

    void values(word result, std::initializer_list<word> want)
    {
        for (auto x : want) {
            if (tag_of(result) != tag::duo) {
                expect(false);
                return;
            }
            const auto [car, cdr] = h.read<tag::duo>(result);
            expect(car == x);
            result = cdr;
        }
        expect(result == nil);
    }
};

static suite eval_tests{
    "WISP EVALUATION", [] {
        "literals, symbol identity, and rooted packages"_test = [] {
            language m;
            expect(m.s("NIL") == nil && m.s("T") == t);
            expect(m.s("X") != m.s("x"));
            root key{m.h, m.vm.keyword("X")};
            expect(key.get() != m.s("X"));
            auto result = m.eval(key.get(), true);
            expect(result == key.get());
            root text{m.h, m.h.newv08("hello")};
            result = m.eval(text.get(), true);
            expect(result == text.get());
            root vector{m.h, m.h.newv32(std::array{fixnum(-8), t})};
            result = m.eval(vector.get(), true);
            expect(result == vector.get());
            expect(m.eval(fixnum(-19)) == fixnum(-19));
            expect(m.eval(nil) == nil && m.eval(t) == t);
            m.eval(m.f("SET-SYMBOL-VALUE!", {m.q(m.s("X")), 73}), true);
            m.h.collect();
            expect(m.eval(m.s("X"), true) == 73u);
        };

        "quote, only NIL is false, and unselected branches are inert"_test =
            [] {
                language m;
                root data{m.h, m.l({m.s("UNBOUND"), 4})};
                const auto result = m.eval(m.q(data.get()), true);
                expect(result == data.get());
                expect(
                    m.eval(m.f("IF", {nil, m.s("UNBOUND"), 29}), true)
                    == 29u);
                expect(
                    m.eval(m.f("IF", {0, 13, m.s("UNBOUND")}), true)
                    == 13u);
                expect(m.eval(m.f("DO"), true) == nil);
                expect(m.eval(m.f("DO", {3, 7, 11}), true) == 11u);
            };

        "arguments run left to right in the caller's environment"_test =
            [] {
                language m;
                auto x = m.s("X");
                auto bump = m.f("%SET!", {m.q(x), m.f("+", {x, 1})});
                auto program =
                    m.f("LET",
                        {m.l({m.l({x, 0})}),
                         m.f("LIST",
                             {bump,
                              m.f("CALL",
                                  {m.fn(m.l({x}), m.f("+", {x, 1})), 90}),
                              bump,
                              x})});
                m.values(m.eval(program, true), {1, 91, 2, 2});
            };

        "LET initializes in parallel scope and preserves binding order"_test =
            [] {
                language m;
                auto x = m.s("X"), y = m.s("Y"), z = m.s("Z");
                auto program = m.f(
                    "LET",
                    {m.l({m.l({x, 40})}),
                     m.f("LIST",
                         {m.f("LET",
                              {m.l({m.l({x, 3}), m.l({y, x}), m.l({z, 9})}),
                               m.f("LIST", {x, y, z})}),
                          x})});
                const auto result = m.eval(program, true);
                m.values(m.h.get<tag::duo, field::car>(result), {3, 40, 9});
                m.values(m.h.get<tag::duo, field::cdr>(result), {40});
                expect(m.eval(m.f("LET", {nil, 18}), true) == 18u);
                expect(m.eval(m.f("LET", {nil}), true) == nil);
            };

        "closures share lexical mutation after their creator returns"_test =
            [] {
                language m;
                auto x = m.s("X"), n = m.s("N");
                auto install = m.f(
                    "LET",
                    {m.l({m.l({x, 10})}),
                     m.f("SET-SYMBOL-FUNCTION!",
                         {m.q(m.s("READ-X")), m.fn(nil, x)}),
                     m.f("SET-SYMBOL-FUNCTION!",
                         {m.q(m.s("ADD-X")),
                          m.fn(
                              m.l({n}),
                              m.f("%SET!", {m.q(x), m.f("+", {x, n})}))})});
                m.eval(install, true);
                auto program = m.f(
                    "LET",
                    {m.l({m.l({m.s("X"), 999})}),
                     m.f("LIST",
                         {m.f("ADD-X", {7}), m.f("READ-X"), m.s("X")})});
                m.values(m.eval(program, true), {17, 17, 999});
                auto get = m.h.get<tag::sym, field::fun>(m.s("READ-X"));
                auto add = m.h.get<tag::sym, field::fun>(m.s("ADD-X"));
                expect(
                    (m.h.get<tag::fun, field::env>(get)
                     == m.h.get<tag::fun, field::env>(add)));
                expect((m.h.get<tag::fun, field::cnt>(get) == 1u));
            };

        "value and function namespaces stay separate and calls see redefinition"_test =
            [] {
                language m;
                auto f = m.s("F");
                auto program = m.f(
                    "DO",
                    {m.f("SET-SYMBOL-VALUE!", {m.q(f), 19}),
                     m.f("SET-SYMBOL-FUNCTION!", {m.q(f), m.fn(nil, 7)}),
                     m.f("LIST",
                         {f,
                          m.f("F"),
                          m.f("DO",
                              {m.f("SET-SYMBOL-FUNCTION!",
                                   {m.q(f), m.fn(nil, 31)}),
                               m.f("F")})})});
                m.values(m.eval(program, true), {19, 7, 31});
                // A computed operator is not implicitly FUNCALL in Wisp.
                auto error = m.error(m.l({m.fn(nil, 3)}));
                expect(m.h.v32slice(error)[0] == m.s("INVALID-CALLEE"));
            };

        "CALL, APPLY, optional parameters, and rest parameters"_test = [] {
            language m;
            auto x = m.s("X"), y = m.s("Y"), rest = m.s("REST");
            auto fun = m.fn(
                m.l({x, m.s("&OPTIONAL"), y, m.s("&REST"), rest}),
                m.f("CONS", {x, m.f("CONS", {y, rest})}));
            // Root source code because it is reused after moving
            // collection.
            root source{m.h, fun};
            m.values(
                m.eval(m.f("CALL", {source.get(), 11}), true), {11, nil});
            m.values(
                m.eval(
                    m.f("APPLY",
                        {source.get(), m.q(m.l({11, 23, 37, 41}))}),
                    true),
                {11, 23, 37, 41});
            expect(
                m.eval(
                    m.f("CALL", {m.f("FUNCTION", {m.s("-")}), 19, 7, 3}),
                    true)
                == 9u);
        };

        "macro arguments are raw and expansions use the caller's scope"_test =
            [] {
                language m;
                auto body = m.s("BODY"), x = m.s("X");
                m.eval(
                    m.f("LET",
                        {m.l({m.l({x, 100})}),
                         m.f("SET-SYMBOL-FUNCTION!",
                             {m.q(m.s("TWICE")),
                              m.f("%MACRO-FN",
                                  {m.l({body}),
                                   m.f("LIST",
                                       {m.q(m.s("+")), body, body})})})}),
                    true);
                x = m.s("X");
                auto bump = m.f("%SET!", {m.q(x), m.f("+", {x, 1})});
                expect(
                    m.eval(
                        m.f("LET",
                            {m.l({m.l({x, 4})}), m.f("TWICE", {bump})}),
                        true)
                    == 11u);
                body = m.s("BODY");
                m.eval(
                    m.f("SET-SYMBOL-FUNCTION!",
                        {m.q(m.s("IGNORE")),
                         m.f("%MACRO-FN",
                             {m.l({m.s("&BODY"), body}), 27})}),
                    true);
                expect(
                    m.eval(
                        m.f("IGNORE",
                            {m.s("UNBOUND"), m.f("NO-SUCH-FUNCTION")}),
                        true)
                    == 27u);
            };

        "fixnums are signed and checked at each arithmetic operation"_test =
            [] {
                language m;
                expect(m.eval(m.f("+")) == 0u);
                expect(m.eval(m.f("*")) == 1u);
                expect(m.eval(m.f("-", {fixnum(-7)})) == fixnum(-7));
                expect(m.eval(m.f("<", {fixnum(-4), 0})) == t);
                expect(m.eval(m.f(">", {fixnum(-4), 0})) == nil);
                expect(m.eval(m.f("*", {fixnum(-7), 3})) == fixnum(-21));
                expect(
                    m.eval(m.f("+", {fixnum(max_fixnum - 1), 1}))
                    == fixnum(max_fixnum));
                expect(
                    m.eval(m.f("-", {fixnum(min_fixnum + 1), 1}))
                    == fixnum(min_fixnum));
                // Final sum would fit, but the first addition overflows.
                auto error = m.cause(
                    m.error(m.f("+", {fixnum(max_fixnum), 1, fixnum(-1)})));
                expect(m.h.v32slice(error)[0] == m.s("FIXNUM-OVERFLOW"));
                error = m.cause(
                    m.error(m.f("*", {fixnum(min_fixnum), fixnum(-1)})));
                expect(m.h.v32slice(error)[0] == m.s("FIXNUM-OVERFLOW"));
                error = m.cause(m.error(m.f("-", {fixnum(min_fixnum), 1})));
                expect(m.h.v32slice(error)[0] == m.s("FIXNUM-OVERFLOW"));
            };

        "failures are inspectable, terminal, and survive collection"_test =
            [] {
                language m;
                auto error = m.error(m.s("MISSING"));
                expect(
                    std::ranges::equal(
                        m.h.v32slice(error),
                        std::array{
                            m.s("UNBOUND-VARIABLE"), m.s("MISSING")}));
                error = m.error(m.f("MISSING"));
                expect(m.h.v32slice(error)[0] == m.s("UNDEFINED-FUNCTION"));
                error = m.cause(m.error(m.f("HEAD", {3})));
                expect(
                    std::ranges::equal(
                        m.h.v32slice(error),
                        std::array{
                            m.s("TYPE-MISMATCH"), m.s("CONS"), word{3}}));
                error = m.cause(m.error(m.f("CONS", {1})));
                expect(
                    m.h.v32slice(error)[1]
                    == m.s("INVALID-ARGUMENT-COUNT"));
                error = m.cause(m.error(m.f("IF", {t, 1})));
                expect(
                    m.h.v32slice(error)[1]
                    == m.s("INVALID-ARGUMENT-COUNT"));
                error = m.cause(m.error(
                    m.f("CALL", {m.fn(m.l({m.s("X")}), m.s("X")), 1, 2})));
                expect(
                    m.h.v32slice(error)[1]
                    == m.s("INVALID-ARGUMENT-COUNT"));
                error = m.cause(m.error(
                    m.f("CALL", {m.fn(m.l({m.s("X")}), m.s("X"))})));
                expect(
                    m.h.v32slice(error)[1]
                    == m.s("INVALID-ARGUMENT-COUNT"));
                error =
                    m.cause(m.error(m.f("%SET!", {m.q(m.s("NEW")), 1})));
                expect(m.h.v32slice(error)[0] == m.s("UNBOUND-VARIABLE"));
                m.h.collect();
                expect(m.vm.status(m.run.get()) == evaluation::failed);
            };

        "malformed lists and parameter lists fail without native assertions"_test =
            [] {
                language m;
                auto error = m.error(m.h.cons(m.s("+"), m.h.cons(1, 2)));
                expect(m.h.v32slice(error)[0] == m.s("TYPE-MISMATCH"));
                auto cycle = m.h.cons(1, nil);
                m.h.set<tag::duo, field::cdr>(cycle, cycle);
                error = m.error(m.h.cons(m.s("LIST"), cycle));
                expect(m.h.v32slice(error)[0] == m.s("CYCLIC-LIST"));
                error = m.cause(
                    m.error(m.f("LET", {m.l({m.l({m.s("X")})}), 1})));
                expect(m.h.v32slice(error)[0] == m.s("INVALID-BINDING"));
                error = m.cause(
                    m.error(m.f("CALL", {m.fn(m.l({m.s("&REST")}), 1)})));
                expect(m.h.v32slice(error)[0] == m.s("INVALID-PARAMETERS"));
            };

        "bounded turns expose argument state and resume after GC"_test =
            [] {
                language m;
                m.run.set(
                    m.vm.start(m.f("LIST", {17, m.f("+", {3, 5}), 29})));
                auto before = m.h.read<tag::run>(m.run.get());
                expect(
                    m.vm.advance(m.run.get(), 0) == evaluation::runnable);
                expect(m.h.read<tag::run>(m.run.get()) == before);
                expect(
                    m.vm.advance(m.run.get(), 3) == evaluation::runnable);
                auto way = m.h.get<tag::run, field::way>(m.run.get());
                auto acc = m.h.get<tag::ktx, field::acc>(way);
                expect(
                    std::ranges::equal(
                        m.h.v32slice(acc),
                        std::array{word{1}, word{17}, nil, nil}));
                for (int i = 0;
                     i < 30
                     && m.vm.status(m.run.get()) == evaluation::runnable;
                     ++i) {
                    m.h.collect();
                    m.vm.step(m.run.get());
                }
                expect(m.vm.status(m.run.get()) == evaluation::done);
                m.values(
                    m.h.get<tag::run, field::val>(m.run.get()),
                    {17, 8, 29});
                before = m.h.read<tag::run>(m.run.get());
                expect(m.vm.step(m.run.get()) == evaluation::done);
                expect(m.h.read<tag::run>(m.run.get()) == before);
            };

        "tail calls have bounded live continuation depth"_test = [] {
            language m;
            auto n = m.s("N"), sum = m.s("SUM");
            auto recur =
                m.f("LOOP", {m.f("-", {n, 1}), m.f("+", {sum, n})});
            auto body =
                m.f("DO", {m.f("IF", {m.f("EQ?", {n, 0}), sum, recur})});
            m.eval(
                m.f("SET-SYMBOL-FUNCTION!",
                    {m.q(m.s("LOOP")), m.fn(m.l({n, sum}), body)}));
            m.run.set(m.vm.start(m.f("LOOP", {1000, 0})));
            std::size_t maximum = 0;
            for (int i = 0;
                 i < 40000
                 && m.vm.status(m.run.get()) == evaluation::runnable;
                 ++i) {
                m.vm.step(m.run.get());
                auto way = m.h.get<tag::run, field::way>(m.run.get());
                std::size_t depth = 0;
                while (way != top) {
                    ++depth;
                    way = m.h.get<tag::ktx, field::hop>(way);
                }
                maximum = std::max(maximum, depth);
                if (i % 127 == 0)
                    m.h.collect();
            }
            expect(m.vm.status(m.run.get()) == evaluation::done);
            expect((m.h.get<tag::run, field::val>(m.run.get()) == 500500u));
            expect(maximum <= 3u);
        };

        "dynamic bindings shadow lexical scope only for marked symbols"_test =
            [] {
                language m;
                auto x = m.s("X");
                m.eval(
                    m.f("DO",
                        {m.f("SET-SYMBOL-VALUE!", {m.q(x), 7}),
                         m.f("SET-SYMBOL-DYNAMIC!", {m.q(x), t}),
                         m.f("SET-SYMBOL-FUNCTION!",
                             {m.q(m.s("READ-X")), m.fn(nil, x)})}),
                    true);
                x = m.s("X");
                auto inner = m.f(
                    "CALL-WITH-BINDING",
                    {m.q(x),
                     20,
                     m.fn(
                         nil,
                         m.f("LIST", {x, m.f("%SET!", {m.q(x), 23})}))});
                auto outer =
                    m.f("CALL-WITH-BINDING",
                        {m.q(x),
                         10,
                         m.fn(
                             nil,
                             m.f("LIST",
                                 {m.f("READ-X"),
                                  inner,
                                  x,
                                  m.f("%SET!", {m.q(x), 14})}))});
                auto program = m.f(
                    "LET", {m.l({m.l({x, 2})}), m.f("LIST", {outer, x})});
                const auto result = m.eval(program, true);
                const auto bound = m.h.get<tag::duo, field::car>(result);
                expect((m.h.get<tag::duo, field::car>(bound) == 10u));
                const auto rest = m.h.get<tag::duo, field::cdr>(bound);
                m.values(m.h.get<tag::duo, field::car>(rest), {20, 23});
                m.values(m.h.get<tag::duo, field::cdr>(rest), {10, 14});
                m.values(m.h.get<tag::duo, field::cdr>(result), {2});
                expect(m.eval(m.s("X"), true) == 7u);
                x = m.s("X");
                expect(
                    m.eval(
                        m.f("DO",
                            {m.f("SET-SYMBOL-DYNAMIC!", {m.q(x), nil}),
                             m.f("CALL-WITH-BINDING",
                                 {m.q(x), 99, m.fn(nil, x)})}),
                        true)
                    == 7u);
            };

        "prompts match by identity and only actual prompt frames match"_test =
            [] {
                language m;
                auto v = m.s("V"), k = m.s("K"), tag = m.s("TAG");
                auto handler = m.fn(m.l({v, k}), m.f("+", {v, 1}));
                auto inner = m.f(
                    "CALL-WITH-PROMPT",
                    {m.q(tag),
                     m.fn(
                         nil, m.f("SEND-WITH-DEFAULT!", {m.q(tag), 7, 90})),
                     handler});
                auto outer = m.f(
                    "CALL-WITH-PROMPT",
                    {m.q(tag), m.fn(nil, inner), m.fn(m.l({v, k}), 99)});
                expect(m.eval(outer, true) == 8u);
                v = m.s("V");
                k = m.s("K");
                tag = m.s("TAG");
                // BINDING.acc equals TAG, but this frame is not a prompt.
                auto binding = m.f(
                    "CALL-WITH-BINDING",
                    {m.q(tag),
                     100,
                     m.fn(
                         nil,
                         m.f("SEND-WITH-DEFAULT!", {m.q(tag), 5, 31}))});
                expect(
                    m.eval(
                        m.f("CALL-WITH-PROMPT",
                            {m.q(tag),
                             m.fn(nil, binding),
                             m.fn(m.l({v, k}), m.f("+", {v, 2}))}),
                        true)
                    == 7u);
                // DO.acc is NIL. Sending to NIL must skip it, too.
                v = m.s("V");
                k = m.s("K");
                auto body = m.f(
                    "DO", {m.f("SEND-WITH-DEFAULT!", {nil, 17, 55}), 99});
                expect(
                    m.eval(
                        m.f("CALL-WITH-PROMPT",
                            {nil, m.fn(nil, body), m.fn(m.l({v, k}), v)}),
                        true)
                    == 17u);
                expect(
                    m.eval(
                        m.f("DO",
                            {m.f("SEND-WITH-DEFAULT!", {nil, 1, 12}), 19}),
                        true)
                    == 19u);
                expect(
                    m.eval(
                        m.f("SEND-WITH-DEFAULT!",
                            {m.q(m.s("MISSING")), 1, 23}),
                        true)
                    == 23u);
                expect(
                    m.eval(
                        m.f("CALL-WITH-PROMPT",
                            {nil,
                             m.fn(nil, 29),
                             m.fn(
                                 m.l({m.s("V"), m.s("K")}),
                                 m.s("UNBOUND"))}),
                        true)
                    == 29u);
            };

        "multi-shot continuations snapshot arguments and compose with the caller"_test =
            [] {
                language m;
                auto saved = m.s("SAVED"), v = m.s("V"), k = m.s("K"),
                     initial = m.s("INITIAL");
                auto body = m.f(
                    "LIST",
                    {11,
                     m.f("SEND-WITH-DEFAULT!", {m.q(m.s("SAVE")), 99, nil}),
                     31});
                auto handler = m.fn(
                    m.l({v, k}),
                    m.f("DO",
                        {m.f("SET-SYMBOL-VALUE!", {m.q(saved), k}), v}));
                auto capture =
                    m.f("CALL-WITH-PROMPT",
                        {m.q(m.s("SAVE")), m.fn(nil, body), handler});
                auto program =
                    m.f("LET",
                        {m.l({m.l({initial, capture})}),
                         m.f("LIST",
                             {initial,
                              m.f("CALL", {saved, 1}),
                              m.f("APPLY", {saved, m.q(m.l({2}))})})});
                const auto result = m.eval(program, true);
                expect((m.h.get<tag::duo, field::car>(result) == 99u));
                auto rest = m.h.get<tag::duo, field::cdr>(result);
                m.values(m.h.get<tag::duo, field::car>(rest), {11, 1, 31});
                rest = m.h.get<tag::duo, field::cdr>(rest);
                m.values(m.h.get<tag::duo, field::car>(rest), {11, 2, 31});
                expect((m.h.get<tag::duo, field::cdr>(rest) == nil));
                auto continuation =
                    m.h.get<tag::sym, field::val>(m.s("SAVED"));
                auto acc = m.h.get<tag::ktx, field::acc>(continuation);
                expect(
                    std::ranges::equal(
                        m.h.v32slice(acc),
                        std::array{word{1}, word{11}, nil, nil}));
                // Keep it only through a host pin, then resume in a fresh
                // run.
                const auto pin = m.h.make_pin(continuation);
                m.h.set<tag::sym, field::val>(m.s("SAVED"), nil);
                m.run.set(nil);
                m.h.collect();
                m.values(
                    m.eval(m.f("CALL", {m.q(m.h.pinned(pin)), 43}), true),
                    {11, 43, 31});
                m.h.free_pin(pin);
            };

        "resumption shares lexical cells but snapshots dynamic binding frames"_test =
            [] {
                language m;
                auto local = m.s("LOCAL"), dyn = m.s("DYN"),
                     saved = m.s("SAVED"), k = m.s("K");
                auto body = m.f(
                    "DO",
                    {m.f("SEND-WITH-DEFAULT!", {m.q(m.s("SAVE")), 0, nil}),
                     m.f("%SET!", {m.q(dyn), m.f("+", {dyn, 1})}),
                     m.f("%SET!", {m.q(local), m.f("+", {local, 1})}),
                     m.f("LIST", {dyn, local})});
                auto binding = m.f(
                    "CALL-WITH-BINDING", {m.q(dyn), 10, m.fn(nil, body)});
                auto handler = m.fn(
                    m.l({m.s("V"), k}),
                    m.f("SET-SYMBOL-VALUE!", {m.q(saved), k}));
                auto program = m.f(
                    "LET",
                    {m.l({m.l({local, 0})}),
                     m.f("SET-SYMBOL-DYNAMIC!", {m.q(dyn), t}),
                     m.f("CALL-WITH-PROMPT",
                         {m.q(m.s("SAVE")), m.fn(nil, binding), handler}),
                     m.f("LIST",
                         {m.f("CALL", {saved, nil}),
                          m.f("CALL", {saved, nil})})});
                const auto result = m.eval(program, true);
                m.values(m.h.get<tag::duo, field::car>(result), {11, 1});
                const auto rest = m.h.get<tag::duo, field::cdr>(result);
                m.values(m.h.get<tag::duo, field::car>(rest), {11, 2});
                expect((m.h.get<tag::duo, field::cdr>(rest) == nil));
            };

        "shallow prompts do not silently reinstall themselves on resumption"_test =
            [] {
                language m;
                auto tag = m.s("ASK"), v = m.s("V"), k = m.s("K");
                auto body =
                    m.f("+",
                        {m.f("SEND-WITH-DEFAULT!", {m.q(tag), 2, 80}),
                         m.f("SEND-WITH-DEFAULT!", {m.q(tag), 3, 7})});
                auto handler =
                    m.fn(m.l({v, k}), m.f("CALL", {k, m.f("*", {v, 10})}));
                expect(
                    m.eval(
                        m.f("CALL-WITH-PROMPT",
                            {m.q(tag), m.fn(nil, body), handler}),
                        true)
                    == 27u);
            };

        "deep handlers can be expressed in Lisp by reinstalling the prompt"_test =
            [] {
                language m;
                auto tag = m.s("TAG"), thunk = m.s("THUNK"),
                     handler = m.s("HANDLER");
                auto request = m.s("REQUEST"), k = m.s("K"),
                     value = m.s("VALUE");
                // The resume half of base.wisp's CALL-WITH-EFFECT-HANDLER,
                // expressed entirely as guest closures rather than a host
                // jet.
                auto resume = m.fn(
                    m.l({value}),
                    m.f("HANDLE",
                        {tag,
                         m.fn(nil, m.f("CALL", {k, value})),
                         handler}));
                auto prompt_handler = m.fn(
                    m.l({request, k}),
                    m.f("CALL", {handler, request, resume}));
                auto handle = m.fn(
                    m.l({tag, thunk, handler}),
                    m.f("CALL-WITH-PROMPT", {tag, thunk, prompt_handler}));
                m.eval(
                    m.f("SET-SYMBOL-FUNCTION!",
                        {m.q(m.s("HANDLE")), handle}),
                    true);
                request = m.s("REQUEST");
                auto resume_sym = m.s("RESUME");
                auto body = m.f(
                    "+",
                    {m.f("SEND-WITH-DEFAULT!", {m.q(m.s("ASK")), 2, 80}),
                     m.f("SEND-WITH-DEFAULT!", {m.q(m.s("ASK")), 3, 7})});
                auto answer = m.fn(
                    m.l({request, resume_sym}),
                    m.f("CALL", {resume_sym, m.f("*", {request, 10})}));
                expect(
                    m.eval(
                        m.f("HANDLE",
                            {m.q(m.s("ASK")), m.fn(nil, body), answer}),
                        true)
                    == 50u);
            };

        "sending to a captured continuation composes both outside contexts"_test =
            [] {
                language m;
                auto v = m.s("V"), k = m.s("K");
                auto send =
                    m.f("SEND-WITH-DEFAULT!", {m.q(m.s("SAVE")), 5, nil});
                auto inner =
                    m.f("CALL-WITH-PROMPT",
                        {m.q(m.s("REMOTE")),
                         m.fn(nil, m.f("+", {100, send})),
                         m.fn(m.l({v, k}), m.f("+", {v, 1}))});
                auto outer_handler = m.fn(
                    m.l({v, k}),
                    m.f("+",
                        {1000,
                         m.f("SEND-TO-WITH-DEFAULT!",
                             {k, m.q(m.s("REMOTE")), 7, 99})}));
                expect(
                    m.eval(
                        m.f("CALL-WITH-PROMPT",
                            {m.q(m.s("SAVE")),
                             m.fn(nil, m.f("+", {30, inner})),
                             outer_handler}),
                        true)
                    == 1038u);
            };

        "language errors are resumable effects and handler errors reach outer prompts"_test =
            [] {
                language m;
                auto e = m.s("E"), k = m.s("K");
                auto body = m.f("LIST", {5, m.s("UNBOUND"), 7});
                auto handler = m.fn(m.l({e, k}), m.f("CALL", {k, 42}));
                m.values(
                    m.eval(
                        m.f("CALL-WITH-PROMPT",
                            {m.q(m.s("ERROR")), m.fn(nil, body), handler}),
                        true),
                    {5, 42, 7});
                e = m.s("E");
                k = m.s("K");
                auto inner =
                    m.f("CALL-WITH-PROMPT",
                        {m.q(m.s("ERROR")),
                         m.fn(nil, m.f("HEAD", {1})),
                         m.fn(m.l({e, k}), m.s("ALSO-UNBOUND"))});
                expect(
                    m.eval(
                        m.f("CALL-WITH-PROMPT",
                            {m.q(m.s("ERROR")),
                             m.fn(nil, inner),
                             m.fn(m.l({e, k}), 73)}),
                        true)
                    == 73u);
            };

        "continuation inspection, TOP identity, and arity are explicit"_test =
            [] {
                language m;
                expect(m.eval(m.f("GET/CC"), true) == top);
                expect(
                    m.eval(m.f("COMPOSE-CONTINUATION", {m.q(top)}), true)
                    == top);
                expect(m.eval(m.f("CALL", {m.q(top), 17}), true) == 17u);
                auto error = m.cause(m.error(m.f("CALL", {m.q(top)})));
                expect(
                    m.h.v32slice(error)[1]
                    == m.s("CONTINUATION-CALL-ERROR"));
                auto v = m.s("V"), k = m.s("K");
                auto handler = m.fn(m.l({v, k}), k);
                auto body =
                    m.f("+",
                        {19, m.f("SEND-WITH-DEFAULT!", {nil, 1, nil}), 23});
                auto captured = m.eval(
                    m.f("CALL-WITH-PROMPT",
                        {nil, m.fn(nil, body), handler}),
                    true);
                root continuation{m.h, captured};
                expect(
                    m.eval(m.f("KTX-POS", {m.q(continuation.get())}), true)
                    == 1u);
                auto fun =
                    m.eval(m.f("KTX-FUN", {m.q(continuation.get())}), true);
                expect((fun == m.h.get<tag::sym, field::fun>(m.s("+"))));
                expect(
                    m.eval(
                        m.f("TOP?",
                            {m.f("KTX-HOP", {m.q(continuation.get())})}),
                        true)
                    == t);
                expect(
                    m.eval(m.f("CALL", {m.q(continuation.get()), 7}), true)
                    == 49u);
                root composed{
                    m.h,
                    m.eval(
                        m.f("COMPOSE-CONTINUATION",
                            {m.q(continuation.get())}),
                        true)};
                expect(composed.get() != continuation.get());
                expect(
                    m.eval(m.f("CALL", {m.q(composed.get()), 13}), true)
                    == 55u);
                error = m.cause(
                    m.error(m.f("CALL", {m.q(continuation.get()), 1, 2})));
                expect(
                    m.h.v32slice(error)[1]
                    == m.s("CONTINUATION-CALL-ERROR"));
                // GET/CC is a live frame reference, not prompt capture.
                // A prompt tag can be an empty vector, with no position.
                auto prompt = m.eval(
                    m.f("CALL-WITH-PROMPT",
                        {m.h.newv32({}), m.fn(nil, m.f("GET/CC")), nil}),
                    true);
                expect(
                    (m.h.get<tag::ktx, field::fun>(prompt)
                     == m.s("PROMPT")));
                expect(m.eval(m.f("KTX-POS", {m.q(prompt)}), true) == 0u);
            };

        "EVAL evaluates a computed form in the current lexical environment"_test =
            [] {
                language m;
                auto x = m.s("X");
                auto program =
                    m.f("LET",
                        {m.l({m.l({x, 19})}),
                         m.f("EVAL", {m.q(m.f("+", {x, 23}))})});
                expect(m.eval(program, true) == 42u);
            };
    }};

} // namespace
} // namespace wisp::test
