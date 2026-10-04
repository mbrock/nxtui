// SPDX-License-Identifier: AGPL-3.0-or-later
#include <wisp/load.hpp>
#include <wisp/nxt.hpp>
#include <wisp/printer.hpp>
#include <nxtrt/app.hpp>

#include "test.hpp"
#include "wisp-base.hpp"

namespace wisp::test {
namespace {

using namespace boost::ut;
using namespace std::chrono_literals;

struct runtime_machine
{
    explicit runtime_machine(std::unique_ptr<image> from = image::fresh())
        : owner(std::move(from))
    {
    }

    std::unique_ptr<image> owner;
    heap & h = owner->storage;
    evaluator & vm = owner->machine;
    root run{h};

    word form(std::string_view text)
    {
        return reader{h, vm, text}.next().value();
    }

    word load(std::string_view text, std::size_t quantum = 257)
    {
        loader source{h, vm, text};
        for (unsigned i = 0; i < 10000; ++i) {
            const auto state = source.advance(quantum);
            vm.collect();
            if (state == evaluation::failed)
                throw std::runtime_error(
                    print(h, h.get<tag::run, field::err>(source.run())));
            if (state == evaluation::done) {
                run.set(source.run());
                return run.get() == nil
                           ? nil
                           : h.get<tag::run, field::val>(run.get());
            }
        }
        throw std::runtime_error("source test exceeded budget");
    }

    void fails(std::string_view text, std::string_view condition)
    {
        try {
            load(text);
            expect(false) << "expected condition " << condition;
        } catch (const std::runtime_error & error) {
            expect(std::string_view{error.what()}.contains(condition))
                << error.what();
        }
    }

    // A run immediately about to invoke STEP!: the value register is its
    // sole evaluated argument, and the application frame is ready to pop.
    word stepping(word target)
    {
        const auto jet = h.get<tag::sym, field::fun>(vm.intern("STEP!"));
        return h.make<tag::run>(
            {nah,
             target,
             nil,
             nil,
             h.make<tag::ktx>({top, nil, jet, nil, nil}),
             top});
    }
};

nxtrt::task<void> tick_twice(int & ticks)
{
    ++ticks;
    co_await nxtrt::yield();
    ++ticks;
}

#if NXTRT_ARCH_HAS_WAND
nxtrt::task<word> host_effect(runtime_machine & m)
{
    m.run.set(m.vm.start(m.form(R"(
        (call-with-prompt 'host
          (%fn nil () (+ 7 (send-with-default! 'host 35 nil)))
          (%fn nil (request resume) (vector request resume))))")));
    if (co_await drive(m.vm, m.run, 3) != evaluation::done)
        throw std::runtime_error("guest request failed");
    root packet{m.h, m.h.get<tag::run, field::val>(m.run.get())};
    m.run.set(nil); // Only the request packet retains the continuation.
    m.vm.collect();
    co_await nxtrt::op::timeout::after(1ms);
    m.vm.collect();
    const auto items = m.h.v32slice(packet.get());
    const auto request = items[0], continuation = items[1];
    const auto quoted =
        m.h.cons(m.vm.intern("QUOTE"), m.h.cons(continuation, nil));
    m.run.set(m.vm.start(m.h.cons(
        m.vm.intern("CALL"), m.h.cons(quoted, m.h.cons(request, nil)))));
    if (co_await drive(m.vm, m.run, 2) != evaluation::done)
        throw std::runtime_error("guest resumption failed");
    co_return m.h.get<tag::run, field::val>(m.run.get());
}
#endif

static suite runtime_tests{
    "WISP RUNTIME", [] {
        "argument snapshots and bindings survive scratch boundaries and GC"_test =
            [] {
                // Binding pairs cross the inline boundary at 17 parameters;
                // argument snapshots cross it at 33 words. Values differ at
                // every position so reversal or truncation cannot pass.
                for (unsigned n : {16, 17, 31, 32, 33, 65}) {
                    runtime_machine m;
                    std::string parameters, arguments;
                    for (unsigned i = 0; i < n; ++i) {
                        parameters += " p" + std::to_string(i);
                        arguments += " " + std::to_string(7 + 3 * i);
                    }
                    const auto function = "(%fn nil (" + parameters
                                          + ") (vector p"
                                          + std::to_string(n - 1) + " p0 p"
                                          + std::to_string(n / 2) + "))";
                    for (const auto & source :
                         {"(call " + function + arguments + ")",
                          "(apply " + function + " '(" + arguments
                              + "))"}) {
                        const auto result = m.load(source, 1);
                        expect(
                            std::ranges::equal(
                                m.h.v32slice(result),
                                std::array{
                                    word{7 + 3 * (n - 1)},
                                    word{7},
                                    word{7 + 3 * (n / 2)}}));
                    }
                }
            };

        "ordinary calls observe same-length syntax edits between arguments"_test =
            [] {
                runtime_machine m;
                expect(
                    print(
                        m.h,
                        m.load(
                            R"(
                (set-symbol-value! 'args
                  '((do (set-head! (tail args) 41) 7) 19))
                (eval (cons 'list args)))",
                            1))
                    == "(7 41)");
            };

        "mutable LET bindings and environments fail as language conditions"_test =
            [] {
                runtime_machine m;
                m.fails(
                    R"(
                (set-symbol-value! 'c '((a (set-tail! (head (tail c)) nil)) (b 2)))
                (eval (list 'let c 'a)))",
                    "INVALID-BINDING");
                m.fails(
                    R"(
                (let ((x 1))
                  (set-head! (env) (vector 'x)) x))",
                    "INVALID-ENVIRONMENT");
                m.fails(
                    R"(
                (let ((x 1))
                  (set-head! (env) 19) x))",
                    "TYPE-MISMATCH");
                m.fails(
                    R"(
                (let ((x 1))
                  (set-tail! (env) (env)) x))",
                    "CYCLIC-LIST");
            };

        "captured application accumulators reject corrupt cursors and changed arity"_test =
            [] {
                runtime_machine m;
                for (auto cursor : {"999", "nil"}) {
                    m.fails(
                        std::string{R"(
                    (+ 11 (do
                      (set-symbol-value! 'k (get/cc))
                      (set-symbol-value! 'k (ktx-hop (ktx-hop k)))
                      (vector-set! (ktx-acc k) 0 )"}
                            + cursor + R"()
                      7) 23))",
                        "INVALID-CONTINUATION");
                }
                m.fails(
                    R"(
                (+ 11 (do
                  (set-symbol-value! 'k (get/cc))
                  (set-symbol-value! 'k (ktx-hop (ktx-hop k)))
                  (set-tail! (ktx-arg k) (list 41))
                  7) 23))",
                    "INVALID-CONTINUATION");
                m.fails(
                    R"(
                (+ 11 (do
                  (set-symbol-value! 'k (get/cc))
                  (set-symbol-value! 'k (ktx-hop (ktx-hop k)))
                  (set-tail! (ktx-arg k) (ktx-arg k))
                  7) 23))",
                    "CYCLIC-LIST");
            };

        "mutable sequence and LET accumulator spines are checked on resumption"_test =
            [] {
                runtime_machine m;
                m.fails(
                    R"(
                (do
                  (set-symbol-value! 'k (get/cc))
                  (set-tail! (tail (ktx-arg (ktx-hop k))) 19)
                  7 23))",
                    "TYPE-MISMATCH");
                m.fails(
                    R"(
                (let ((x (do
                  (set-symbol-value! 'k (get/cc))
                  (set-symbol-value! 'k (ktx-hop (ktx-hop k)))
                  (set-tail! (ktx-acc k) nil)
                  7))) x))",
                    "INVALID-CONTINUATION");
            };

        "package lookup is own-first, ordered, direct-only, and rooted"_test =
            [] {
                runtime_machine m;
                root a{m.h, m.vm.define_package("A")};
                root b{m.h, m.vm.define_package("B")};
                root c{m.h, m.vm.define_package("C")};
                root ax{m.h, m.vm.intern("X", a.get())};
                root bx{m.h, m.vm.intern("X", b.get())};
                root local{m.h, m.vm.intern("LOCAL", c.get())};
                m.vm.intern("LOCAL", a.get());
                root uses{m.h, m.h.cons(b.get(), m.h.cons(a.get(), nil))};
                m.h.set<tag::pkg, field::use>(c.get(), uses.get());
                expect(m.vm.intern("X", c.get()) == bx.get());
                expect(m.vm.intern("LOCAL", c.get()) == local.get());
                m.h.set<tag::duo, field::car>(uses.get(), a.get());
                expect(m.vm.intern("X", c.get()) == ax.get());
                root d{m.h, m.vm.define_package("D")};
                m.h.set<tag::pkg, field::use>(
                    d.get(), m.h.cons(c.get(), nil));
                const auto dx = m.vm.intern("X", d.get());
                expect((m.h.get<tag::sym, field::pkg>(dx) == d.get()));
                m.h.set<tag::pkg, field::use>(
                    a.get(), m.h.cons(c.get(), nil));
                const auto fresh = m.vm.intern("FRESH", c.get());
                expect((m.h.get<tag::sym, field::pkg>(fresh) == c.get()));
                m.vm.collect();
                expect(m.vm.find_package("C") == c.get());
                expect(m.vm.find_package("c") == nil);
                expect(m.vm.intern("X", c.get()) == ax.get());
                // WISP's NIL/T special case does not become an inherited
                // symbol.
                expect(m.vm.intern("NIL", c.get()) != nil);
                expect(m.vm.intern("T", c.get()) != t);
                m.fails("(%defpackage \"C\")", "PACKAGE-EXISTS");
                m.load("(set-tail! (packages) nil)");
                expect(m.vm.find_package("A") == a.get());
            };

        "uses mutations validate before changing the package and malformed aliases fail"_test =
            [] {
                runtime_machine m;
                m.load("(%defpackage \"P\")");
                m.fails(
                    "(package-set-uses! (find-package \"P\") '(nil))",
                    "TYPE-MISMATCH");
                m.fails(
                    "(package-set-uses! (find-package \"P\") '(1 . 2))",
                    "TYPE-MISMATCH");
                expect(
                    m.load("(package-uses (find-package \"P\"))") == nil);
                m.load(
                    "(set-symbol-value! 'uses (list (find-package \"WISP\"))) (package-set-uses! (find-package \"P\") uses) (set-tail! uses uses)");
                m.fails(
                    "(intern \"NOT-IN-WISP\" (find-package \"P\"))",
                    "INVALID-PACKAGE-USES");
                m.fails(
                    "(package-set-uses! (find-package \"P\") uses)",
                    "CYCLIC-LIST");
                m.load("(in-package p)");
                try {
                    m.form("brand-new");
                    expect(false);
                } catch (const read_error & error) {
                    expect(error.offset == 0u);
                }
            };

        "loading changes reader package between forms, not within already read forms"_test
            .with_timeout(10s) = [] {
            runtime_machine m{base_image()};
            expect(
                m.load(R"(
            (defpackage meadow (:use wisp))
            (in-package meadow)
            (defun answer (x) (+ x 7))
            (gc)
            (answer 35))")
                == 42u);
            expect(m.vm.current_package() == m.vm.find_package("MEADOW"));
            expect(
                m.h.v08slice(m.load("(print-to-string 'answer)"))
                == "ANSWER");
            expect(
                m.h.v08slice(m.load("(print-to-string 'wisp:head)"))
                == "WISP:HEAD");
            m.load("(in-package wisp)");
            expect(m.load("(meadow:answer 13)") == 20u);
            m.fails("(in-package nowhere)", "UNDEFINED-PACKAGE");
            expect(m.vm.current_package() == m.vm.find_package("WISP"));
            m.load(
                "(do (in-package meadow) (set-symbol-value! 'already-read 19))");
            expect(
                (m.h.get<tag::sym, field::val>(m.vm.intern("ALREADY-READ"))
                 == 19u));
            expect(
                m.vm.intern("ALREADY-READ", m.vm.find_package("MEADOW"))
                == m.vm.intern("ALREADY-READ"));
        };

        "GC requests stop a budget without collecting live scratch registers"_test =
            [] {
                runtime_machine m;
                m.load("(set-symbol-value! 'later 7)");
                m.run.set(m.vm.start(
                    m.form("(do (gc) (set-symbol-value! 'later 19))")));
                const auto era = m.h.era();
                expect(
                    m.vm.advance(m.run.get(), 10000)
                    == evaluation::runnable);
                expect(m.vm.collection_requested());
                expect(m.h.era() == era);
                expect(
                    (m.h.get<tag::sym, field::val>(m.vm.intern("LATER"))
                     == 7u));
                const auto before = m.h.read<tag::run>(m.run.get());
                m.vm.advance(m.run.get(), 100);
                expect(m.h.read<tag::run>(m.run.get()) == before);
                m.vm.collect();
                expect(!m.vm.collection_requested() && m.h.era() != era);
                expect(m.vm.advance(m.run.get(), 100) == evaluation::done);
                expect((m.h.get<tag::run, field::val>(m.run.get()) == 19u));
                expect(
                    print(
                        m.h,
                        m.load(
                            "(let ((v (vector 'kept))) (gc) (list v (gc) v))"))
                    == "(#<KEPT> NIL #<KEPT>)");
            };

        "batched run reflection sees the current transition entry"_test =
            [] {
                for (const auto quantum : {1u, 7u, 4096u}) {
                    for (const auto expression :
                         {"(run-exp self)",
                          "(run-val self)",
                          "(run-way self)",
                          "(call (function run-way) self)",
                          "(apply (function run-way) (list self))"}) {
                        runtime_machine m;
                        m.run.set(m.vm.start(m.form(expression)));
                        m.h.set<tag::sym, field::val>(
                            m.vm.intern("SELF"), m.run.get());
                        auto state = evaluation::runnable;
                        for (unsigned n = 0;
                             n < 100 && state == evaluation::runnable;
                             ++n)
                            state = m.vm.advance(m.run.get(), quantum);
                        expect(state == evaluation::done) << expression;
                        const auto value =
                            m.h.get<tag::run, field::val>(m.run.get());
                        const auto text = std::string_view{expression};
                        if (text == "(run-exp self)") {
                            expect(
                                m.h.read<tag::duo>(value)
                                == row<tag::duo>{
                                    m.vm.known("VAL"), m.run.get()});
                        } else if (text == "(run-val self)") {
                            expect(value == m.run.get());
                        } else {
                            const auto name =
                                text.starts_with("(call")    ? "CALL"
                                : text.starts_with("(apply") ? "APPLY"
                                                             : "RUN-WAY";
                            expect(tag_of(value) == tag::ktx);
                            expect(
                                (m.h.get<tag::ktx, field::fun>(value)
                                 == m.h.get<tag::sym, field::fun>(
                                     m.vm.intern(name))));
                            expect(m.h.continuation_frozen(value));
                        }
                    }
                }
            };

        "self observation preserves frozen and writable entry progress"_test =
            [] {
                for (bool frozen : {false, true}) {
                    runtime_machine m;
                    const auto observer = m.h.get<tag::sym, field::fun>(
                        m.vm.intern("RUN-WAY"));
                    const auto caller =
                        m.h.get<tag::sym, field::fun>(m.vm.intern("CALL"));
                    const auto progress =
                        m.h.newv32(std::array{word{1}, observer, nil});
                    const auto frame = m.h.make<tag::ktx>(
                        {top, nil, caller, progress, nil});
                    m.run.set(m.h.make<tag::run>(
                        {nah, nil, nil, nil, frame, top}));
                    m.h.set<tag::run, field::val>(m.run.get(), m.run.get());
                    if (frozen)
                        m.h.freeze_continuations();
                    expect(
                        m.vm.advance(m.run.get(), 4096)
                        == evaluation::done);
                    expect(
                        (m.h.get<tag::run, field::val>(m.run.get())
                         == frame));
                    expect(m.h.v32slice(progress)[0] == (frozen ? 1u : 2u));
                    expect(
                        m.h.v32slice(progress)[2]
                        == (frozen ? nil : m.run.get()));
                }
            };

        "nested STEP! observers see committed ancestors before batching resumes"_test =
            [] {
                runtime_machine m;
                m.run.set(m.vm.start(
                    m.form("(do (+ 2 3) (step! child) (+ 11 17))")));
                const auto observer =
                    m.h.get<tag::sym, field::fun>(m.vm.intern("RUN-VAL"));
                root child{
                    m.h,
                    m.h.make<tag::run>(
                        {nah,
                         m.run.get(),
                         nil,
                         nil,
                         m.h.make<tag::ktx>({top, nil, observer, nil, nil}),
                         top})};
                m.h.set<tag::sym, field::val>(
                    m.vm.intern("CHILD"), child.get());
                expect(m.vm.advance(m.run.get(), 4096) == evaluation::done);
                expect(
                    (m.h.get<tag::run, field::val>(m.run.get())
                     == fixnum(28)));
                expect(m.vm.status(child.get()) == evaluation::done);
                expect((m.h.get<tag::run, field::val>(child.get()) == nil));
            };

        "STEP! advances once, isolates failure, and rejects active run cycles"_test =
            [] {
                runtime_machine m;
                root target{m.h, m.vm.start(m.form("(+ 11 31)"))};
                m.run.set(m.stepping(target.get()));
                expect(m.vm.step(m.run.get()) == evaluation::done);
                expect(m.vm.status(target.get()) == evaluation::runnable);
                expect(
                    (m.h.get<tag::run, field::exp>(target.get()) == 11u));
                expect(m.vm.advance(target.get(), 30) == evaluation::done);
                const auto finished = m.h.read<tag::run>(target.get());
                m.run.set(m.stepping(target.get()));
                expect(m.vm.step(m.run.get()) == evaluation::done);
                expect(m.h.read<tag::run>(target.get()) == finished);
                target.set(m.vm.start(m.form("unbound")));
                m.run.set(m.stepping(target.get()));
                expect(m.vm.step(m.run.get()) == evaluation::done);
                expect(m.vm.status(target.get()) == evaluation::failed);
                target.set(m.stepping(nil));
                m.h.set<tag::run, field::val>(target.get(), target.get());
                expect(m.vm.step(target.get()) == evaluation::failed);
                expect(
                    print(m.h, m.h.get<tag::run, field::err>(target.get()))
                        .contains("ACTIVE-EVALUATOR"));
                root other{m.h, m.stepping(target.get())};
                m.h.put<tag::run>(
                    target.get(),
                    m.h.read<tag::run>(m.stepping(other.get())));
                expect(m.vm.step(target.get()) == evaluation::done);
                expect(m.vm.status(other.get()) == evaluation::failed);
                m.fails("(step! 1)", "TYPE-MISMATCH");
            };

        "nested STEP! retains active ancestors across scratch growth"_test =
            [] {
                for (unsigned n : {15, 16, 17, 32, 33, 65}) {
                    runtime_machine m;
                    root leaf{m.h, m.stepping(nil)};
                    auto outer = leaf.get();
                    for (unsigned i = 1; i < n; ++i)
                        outer = m.stepping(outer);
                    m.run.set(outer);
                    m.h.set<tag::run, field::val>(leaf.get(), outer);
                    expect(m.vm.step(outer) == evaluation::done);
                    expect(m.vm.status(leaf.get()) == evaluation::failed);
                    m.vm.collect();
                    expect(
                        print(
                            m.h, m.h.get<tag::run, field::err>(leaf.get()))
                            .contains("ACTIVE-EVALUATOR"));
                }
            };

        "nested STEP! chains commit before servicing a GC request"_test =
            [] {
                runtime_machine m;
                root target{m.h, m.vm.start(m.form("(gc)"))};
                auto outer = target.get();
                for (int i = 0; i < 2000; ++i)
                    outer = m.stepping(outer);
                m.run.set(outer);
                expect(m.vm.step(m.run.get()) == evaluation::done);
                expect(m.vm.status(target.get()) == evaluation::done);
                expect(m.vm.collection_requested());
                m.vm.collect();
                expect((m.h.get<tag::run, field::val>(m.run.get()) == nil));
                expect(
                    (m.h.get<tag::run, field::val>(target.get()) == nil));
            };

        "NXT turns yield fairly, retain roots, and service guest collection"_test =
            [] {
                runtime_machine m;
                m.run.set(m.vm.start(m.form("(do (gc) (+ 19 23))")));
                nxtrt::deck deck;
                nxtrt::root_task owner{
                    deck, [&] { return drive(m.vm, m.run, 1); }};
                auto & task = owner.inner();
                int ticks = 0;
                nxtrt::root_task sibling_owner{
                    deck, [&] { return tick_twice(ticks); }};
                auto & sibling = sibling_owner.inner();
                m.vm.collect(); // Task has not started; its input must not
                                // be a stale word.
                deck.start(task);
                deck.start(sibling);
                deck.run_ready();
                expect(ticks == 1 && !task.done());
                for (unsigned turns = 0; turns < 100 && !task.done();
                     ++turns) {
                    m.vm.collect();
                    deck.run_ready();
                }
                expect(ticks == 2 && sibling.done() && task.done());
                expect(std::move(task).result() == evaluation::done);
                expect((m.h.get<tag::run, field::val>(m.run.get()) == 42u));
                expect(!m.vm.collection_requested());
            };

        "NXT cancellation leaves a resumable run and rejects zero quantum"_test =
            [] {
                runtime_machine m;
                m.run.set(m.vm.start(m.form("(+ 17 25)")));
                nxtrt::deck deck;
                nxtrt::root_task owner{
                    deck, [&] { return drive(m.vm, m.run, 1); }};
                auto & task = owner.inner();
                deck.start(task);
                deck.run_ready();
                const auto stopped = m.h.read<tag::run>(m.run.get());
                task.request_stop();
                deck.run_until_idle();
                try {
                    std::move(task).result();
                    expect(false);
                } catch (const nxtrt::operation_cancelled &) {
                }
                expect(m.h.read<tag::run>(m.run.get()) == stopped);
                expect(deck.sync_wait([&] {
                    return drive(m.vm, m.run, 2);
                }) == evaluation::done);
                expect((m.h.get<tag::run, field::val>(m.run.get()) == 42u));
                try {
                    (void) deck.sync_wait(
                        [&] { return drive(m.vm, m.run, 0); });
                    expect(false);
                } catch (const std::invalid_argument &) {
                }
            };

#if NXTRT_ARCH_HAS_WAND
        "an NXT timer completion resumes a rooted guest request"_test = [] {
            runtime_machine m;
            nxtrt::runtime rt;
            expect(rt.run([&] { return host_effect(m); }) == 42u);
        };
#endif
    }};

} // namespace
} // namespace wisp::test
