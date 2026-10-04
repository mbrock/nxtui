// SPDX-License-Identifier: AGPL-3.0-or-later
// Compiler tests that need native APIs. Most compiler tests and the
// semantic corpus are Wisp files in test/wisp/, run by
// wisp-script-test.cpp.
#include <wisp/printer.hpp>
#include <wisp/code.hpp>

#include "test.hpp"
#include "wisp-source.hpp"

namespace wisp::test {
namespace {

using namespace boost::ut;

static suite compiler_tests{
    "WISP COMPILER", [] {
        "analyzed graphs keep their binding identity in a tape"_test = [] {
            source_machine m{compiler_image()};
            m.load(R"((defvar x (make-ir-binding 'x nil))
                      (defvar node (analyze '(if x (foo x) 17) (list x))))");
            source_machine copy{tape::decode(tape::encode(m.vm))};
            copy.check(
                R"((list (ir-show node)
                         (eq? x (ir-reference-binding (ir-branch-test node)))
                         (eq? (record-type node) <ir-branch>)))",
                "((:IF (:REFERENCE X 1) (:CALL FOO (:REFERENCE X 1)) "
                "(:CONSTANT 17)) T T)");
        };

        "functions, scopes, and captures survive a tape"_test = [] {
            source_machine m{compiler_image()};
            m.load(R"((defvar node
                        (analyze '(let ((n 0) (y 1))
                                    (fn (step &rest notes)
                                      (set! n (+ n step))
                                      (list n notes (%macro-fn () y))))))
                      (defvar shown (ir-show node)))");
            source_machine copy{tape::decode(tape::encode(m.vm))};
            copy.check(
                R"((let* ((closure (ir-let-body node))
                          (function (ir-closure-function closure))
                          (n (vector-get (ir-let-bindings node) 0)))
                     (list (equal? (ir-show node) shown)
                           (ir-check node)
                           (eq? node (ir-binding-owner n))
                           (eq? function
                                (ir-binding-owner
                                 (ir-parameters-rest
                                  (ir-function-parameters function))))
                           (map #'ir-binding-name (ir-captures closure))
                           (eq? n (head (ir-captures closure))))))",
                "(T NIL T T (N Y) T)");
        };

        "a prepared call suspended between arguments resumes from tapes"_test =
            [] {
                source_machine m{compiler_image()};
                m.load(R"((defvar saved nil)
                          (defvar program
                            (analyze
                             '(call-with-prompt 'pause
                                (fn ()
                                  (let ((x 0))
                                    (list (do (set! x (+ x 1)) x)
                                          (send! 'pause)
                                          (do (set! x (+ x 1)) x))))
                                (fn (v k) (set! saved k) 'paused)))))");
                // Run the prepared program with a collection between every
                // transition, then save the machine before any resumption.
                m.check(R"((eval program))", "PAUSED");
                expect(m.load("(eval program)", 1) != nil);
                const auto saved = tape::encode(m.vm);
                // Each restored copy is its own machine: resuming twice
                // inside one shares its lexical store, and copies never
                // share theirs.
                for (int copy = 0; copy < 2; ++copy) {
                    source_machine restored{tape::decode(saved)};
                    expect(
                        print(
                            restored.h,
                            restored.load(
                                "(list (call saved 10) (call saved 20))",
                                1))
                        == "((1 10 2) (1 20 3))");
                }
                m.check(
                    "(list (call saved 10) (call saved 20))",
                    "((1 10 2) (1 20 3))");
            };

        "a lowered call suspended between arguments resumes from tapes"_test =
            [] {
                source_machine m{compiler_image()};
                m.load(R"((defvar saved nil)
                          (defvar program
                            (lower
                             (analyze
                              '(call-with-prompt 'pause
                                 (fn ()
                                   (let ((x 0))
                                     (list (do (set! x (+ x 1)) x)
                                           (send! 'pause)
                                           (do (set! x (+ x 1)) x))))
                                 (fn (v k) (set! saved k) 'paused))))))");
                expect(print(m.h, m.load("(eval program)", 1)) == "PAUSED");
                expect(m.load("(eval program)", 1) != nil);
                const auto saved = tape::encode(m.vm);
                for (int copy = 0; copy < 2; ++copy) {
                    source_machine restored{tape::decode(saved)};
                    expect(
                        print(
                            restored.h,
                            restored.load(
                                "(list (call saved 10) (call saved 20))",
                                1))
                        == "((1 10 2) (1 20 3))");
                }
                // Restored frames validate cursors and saved payloads.
                for (bool bad_cursor : {false, true}) {
                    source_machine restored{tape::decode(saved)};
                    std::size_t damaged = 0;
                    for (word i = 0;
                         i < restored.h.table<tag::ktx>().size();
                         ++i) {
                        const auto frame =
                            pointer(tag::ktx, i, restored.h.era());
                        const auto node =
                            restored.h.get<tag::ktx, field::fun>(frame);
                        if (tag_of(node) != tag::rec)
                            continue;
                        const auto xs = restored.h.words<tag::rec>(node);
                        if (xs.size() != 3
                            || xs[0] != code_opcode(code_op::call)
                            || xs[1] != restored.vm.intern("LIST"))
                            continue;
                        ++damaged;
                        if (bad_cursor)
                            restored.h.set<tag::ktx, field::arg>(
                                frame, fixnum(99));
                        else
                            restored.h.set<tag::ktx, field::acc>(
                                frame, nil);
                    }
                    expect(damaged > 0u);
                    restored.check(
                        "(try (call saved 10) (catch (e k) (type-of e)))",
                        "INVALID-CONTINUATION");
                }
                m.check(
                    "(list (call saved 10) (call saved 20))",
                    "((1 10 2) (1 20 3))");
            };

        "lowered code releases IR bindings and needs no descriptors"_test =
            [] {
                source_machine m{compiler_image()};
                m.load(R"((defvar graph
                        (analyze '(let ((x 41)) (fn (y) (+ x y)))))
                      (defvar program (lower graph)))");
                const auto bindings = [&] {
                    const auto descriptor = m.h.get<tag::sym, field::val>(
                        m.vm.intern("<IR-BINDING>"));
                    std::size_t count = 0;
                    for (word i = 0; i < m.h.table<tag::rec>().size();
                         ++i) {
                        const auto xs = m.h.words<tag::rec>(
                            pointer(tag::rec, i, m.h.era()));
                        if (!xs.empty() && xs[0] == descriptor)
                            ++count;
                    }
                    return count;
                };
                expect(
                    bindings() == 2u); // One LET and one parameter binder.
                m.load("(set! graph nil)");
                m.h.collect();
                expect(bindings() == 0u);
                // Make all IR descriptors unrecognizable to the record
                // decoder. The source base library and code inspection
                // still work.
                for (auto name :
                     {"BINDING",
                      "REFERENCE",
                      "LOOKUP",
                      "FUNCTION-REFERENCE",
                      "CONSTANT",
                      "ASSIGNMENT",
                      "CALL",
                      "BRANCH",
                      "SEQUENCE",
                      "LET",
                      "FUNCTION",
                      "PARAMETERS",
                      "CLOSURE",
                      "SOURCE"}) {
                    const auto descriptor = m.h.get<tag::sym, field::val>(
                        m.vm.intern(std::string{"<IR-"} + name + ">"));
                    m.h.set_word<tag::rec>(descriptor, 1, nil);
                }
                m.check("(call (eval program) 1)", "42");
                m.check("(head (code-show program))", ":LET");
            };
    }};

} // namespace
} // namespace wisp::test
