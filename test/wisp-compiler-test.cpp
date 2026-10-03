// SPDX-License-Identifier: AGPL-3.0-or-later
// Compiler tests that need native APIs. Most compiler tests and the
// semantic corpus are Wisp files in test/wisp/, run by wisp-script-test.cpp.
#include <wisp/printer.hpp>

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
                                "(list (call saved 10) (call saved 20))", 1))
                        == "((1 10 2) (1 20 3))");
                }
                m.check(
                    "(list (call saved 10) (call saved 20))",
                    "((1 10 2) (1 20 3))");
            };
    }};

} // namespace
} // namespace wisp::test
