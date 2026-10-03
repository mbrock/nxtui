// SPDX-License-Identifier: AGPL-3.0-or-later
// The semantic corpus for RFC 0020's prepared execution. Each case is a
// small program that runs in its own copy of the base image, so multi-shot
// mutation cannot leak between cases. Today only source mode runs; prepared
// and bytecode modes will run the same table under their own contracts.
#include <wisp/printer.hpp>

#include "test.hpp"
#include "wisp-source.hpp"

#include <array>
#include <string_view>

namespace wisp::test {
namespace {

using namespace boost::ut;

/// One corpus program. `source` is what the source interpreter prints for
/// the last form. `prepared` is set only where RFC 0020's liveness contract
/// deliberately differs from source interpretation; `differs` says why.
struct corpus_case
{
    std::string_view name;
    std::string_view program;
    std::string_view source;
    std::string_view prepared = {};
    std::string_view differs = {};
};

// The order is the corpus numbering; append new cases within a topic.
constexpr std::array corpus{
    // Values and evaluation order.
    corpus_case{
        "constants and quotation evaluate to themselves",
        R"((list 1 "two" 'three nil t :key '(4 5)))",
        R"((1 "two" THREE NIL T :KEY (4 5)))"},
    corpus_case{
        "arguments are evaluated left to right",
        R"((let ((log nil))
             (list (do (set! log (cons 1 log)) 'a)
                   (do (set! log (cons 2 log)) 'b))
             (reverse log)))",
        "(1 2)"},
    corpus_case{
        "value and function namespaces are distinct",
        R"((defun x () 'function)
           (let ((x 'value)) (list x (x) (call #'x))))",
        "(VALUE FUNCTION FUNCTION)"},

    // Lexical binding.
    corpus_case{
        "inner bindings shadow outer ones only in their body",
        R"((let ((x 1)) (list x (let ((x 2)) x) x)))",
        "(1 2 1)"},
    corpus_case{
        "LET initializers see the enclosing scope",
        R"((let ((x 1)) (let ((x 2) (y x)) (list x y))))",
        "(2 1)"},
    corpus_case{
        "duplicate names in one LET resolve to the last",
        R"((let ((x 1) (x 2)) x))",
        "2"},
    corpus_case{
        "duplicate parameter names resolve to the first",
        R"((call (fn (x x) x) 1 2))",
        "1"},
    corpus_case{
        "optional parameters default to NIL and rest collects a list",
        R"((defun f (a &optional b &rest cs) (list a b cs))
           (list (f 1) (f 1 2) (f 1 2 3 4)))",
        "((1 NIL NIL) (1 2 NIL) (1 2 (3 4)))"},
    corpus_case{
        "missing required arguments signal a program error",
        R"((defun needs-one (a) a)
           (try (needs-one) (catch (e k) (type-of e))))",
        "PROGRAM-ERROR"},

    // Closures and shared locations.
    corpus_case{
        "closures share one mutable location",
        R"((let ((n 0))
             (let ((inc (fn () (set! n (+ n 1))))
                   (get (fn () n)))
               (call inc)
               (call inc)
               (call get))))",
        "2"},
    corpus_case{
        "each closure creation captures a fresh location",
        R"((defun make-counter ()
             (let ((n 0)) (fn () (set! n (+ n 1)))))
           (let ((a (make-counter)) (b (make-counter)))
             (call a) (call a)
             (list (call a) (call b))))",
        "(3 1)"},
    corpus_case{
        "assignment reaches through intervening closures",
        R"((let ((x 1))
             (call (fn () (call (fn () (set! x 5)))))
             x))",
        "5"},

    // Dynamic scope.
    corpus_case{
        "dynamic bindings are visible to callees and restored after",
        R"((defparameter *depth* 1)
           (defun read-depth () *depth*)
           (list (read-depth)
                 (binding ((*depth* 2)) (read-depth))
                 (read-depth)))",
        "(1 2 1)"},
    corpus_case{
        "explicit lexical binders stay lexical after a dynamic declaration",
        R"((defvar y 1)
           (let ((f (let ((y 10)) (fn () y))))
             (set-symbol-dynamic! 'y t)
             (call f)))",
        "10"},

    // Redefinition, following the liveness contract.
    corpus_case{
        "the callee is resolved before its arguments run",
        R"((defun target (x) (list 'old x))
           (list (target (do (defun target (x) (list 'new x)) 1))
                 (target 2)))",
        "((OLD 1) (NEW 2))"},
    corpus_case{
        "redefining a function updates existing callers",
        R"((defun callee () 1)
           (defun caller () (callee))
           (defun callee () 2)
           (caller))",
        "2"},
    corpus_case{
        "DEFUN bodies keep the macro expansion from definition time",
        R"((defmacro early () 1)
           (defun uses-early () (early))
           (defmacro early () 2)
           (uses-early))",
        "1"},
    corpus_case{
        "unexpanded bodies see a redefined macro",
        R"((defmacro late () 1)
           (set-symbol-function! 'uses-late (%fn uses-late () (late)))
           (defmacro late () 2)
           (uses-late))",
        "2",
        "1",
        "preparation expands macros once"},
    corpus_case{
        "a function that becomes a macro during its arguments is still called",
        R"((defun shape (x) (list 'function x))
           (shape (do (defmacro shape (x) (list 'quote (list 'macro x))) 1)))",
        "(FUNCTION 1)"},
    corpus_case{
        "a caller meets a function that has become a macro",
        R"((defun shifty (x) (list 'function x))
           (set-symbol-function! 'calls-shifty (%fn calls-shifty () (shifty 5)))
           (defmacro shifty (x) (list 'quote (list 'macro x)))
           (try (calls-shifty) (catch (e k) (type-of e))))",
        "(MACRO 5)",
        "INVALID-FUNCTION",
        "a prepared call site signals instead of expanding"},
    corpus_case{
        "editing a function's source conses changes its behavior",
        R"((defun edit-me () (+ 1 2))
           (set-head! (tail (code #'edit-me)) 10)
           (edit-me))",
        "12",
        "3",
        "prepared code snapshots its syntax"},

    // Reflection and evaluation scope.
    corpus_case{
        "ENV returns the actual lexical store",
        R"((let ((x 1)) (eq? (env) (env))))",
        "T"},
    corpus_case{
        "assignment through ENV is visible to the code",
        R"((let ((x 1))
             (vector-set! (head (env)) 1 42)
             x))",
        "42"},
    corpus_case{
        "public EVAL excludes caller locals",
        R"((defvar z 'global)
           (let ((z 'local)) (eval 'z)))",
        "GLOBAL"},
    corpus_case{
        "macro expansions see call-site locals",
        R"((defmacro get-x () 'x)
           (let ((x 7)) (get-x)))",
        "7"},

    // Conditions.
    corpus_case{
        "primitive type failures signal conditions",
        R"((try (+ 1 'a)
             (catch (e k) (list (type-of e) (type-of (record-get e 1))))))",
        "(BUILTIN-FAILURE TYPE-MISMATCH)"},
    corpus_case{
        "fixnum overflow signals a condition",
        R"((try (* 1073741823 2)
             (catch (e k) (list (type-of e) (type-of (record-get e 1))))))",
        "(BUILTIN-FAILURE FIXNUM-OVERFLOW)"},

    // Control.
    corpus_case{
        "deep self tail recursion completes",
        R"((defun count-down (n) (if (eq? n 0) 'done (count-down (- n 1))))
           (count-down 20000))",
        "DONE"},
    corpus_case{
        "a call suspended between arguments resumes twice",
        R"((defvar saved nil)
           (defvar initial
             (call-with-prompt 'pause
               (fn ()
                 (let ((x 0))
                   (list (do (set! x (+ x 1)) x)
                         (send! 'pause)
                         (do (set! x (+ x 1)) x))))
               (fn (v k) (set! saved k) 'paused)))
           (list initial (call saved 10) (call saved 20)))",
        "(PAUSED (1 10 2) (1 20 3))"},
    corpus_case{
        "resumptions share lexical locations but not dynamic bindings",
        R"((defparameter *p* 0)
           (defvar again nil)
           (defvar initial
             (call-with-prompt 'grab
               (fn ()
                 (let ((n 0))
                   (binding ((*p* 1))
                     (send! 'grab)
                     (set! n (+ n 1))
                     (set! *p* (+ *p* 1))
                     (list n *p*))))
               (fn (v k) (set! again k) 'grabbed)))
           (list initial (call again nil) (call again nil) *p*))",
        "(GRABBED (1 2) (2 2) 0)"},
};

static suite compiler_tests{
    "WISP COMPILER", [] {
        "the semantic corpus in source mode"_group = [] {
            for (const auto & c : corpus)
                test_case{c.name} = [&c] {
                    source_machine m{base_image()};
                    m.check(c.program, c.source);
                };
        };

        "corpus cases that differ in prepared mode say why"_test = [] {
            for (const auto & c : corpus)
                expect(c.prepared.empty() == c.differs.empty()) << c.name;
        };

        "analysis"_group = [] {
            "IR nodes are DEFSTRUCT records"_test = [] {
                source_machine m{compiler_image()};
                m.check(
                    R"((list (type-of (analyze 1))
                             (type-of (analyze 'x))
                             (ir-constant? (analyze "s"))))",
                    "(IR-CONSTANT IR-LOOKUP T)");
            };

            "self-evaluating atoms and quotations become constants"_test =
                [] {
                    source_machine m{compiler_image()};
                    m.check(
                        R"((ir-show
                             (analyze '(list 1 "two" :three nil t 'four '(5)))))",
                        R"((:CALL LIST (:CONSTANT 1) (:CONSTANT "two") (:CONSTANT :THREE) (:CONSTANT NIL) (:CONSTANT T) (:CONSTANT FOUR) (:CONSTANT (5))))");
                };

            "both uses of a binding share one binding object"_test = [] {
                source_machine m{compiler_image()};
                m.check(
                    R"((let* ((x (make-ir-binding 'x nil))
                              (node (analyze '(if x (foo x) 17) (list x)))
                              (call-node (ir-branch-consequent node)))
                         (list (ir-show node)
                               (eq? x (ir-reference-binding
                                       (ir-branch-test node)))
                               (eq? x (ir-reference-binding
                                       (vector-get
                                        (ir-call-arguments call-node)
                                        0))))))",
                    "((:IF (:REFERENCE X 1) (:CALL FOO (:REFERENCE X 1)) "
                    "(:CONSTANT 17)) T T)");
            };

            "the innermost binding of a name wins"_test = [] {
                source_machine m{compiler_image()};
                m.check(
                    R"((let* ((outer (make-ir-binding 'x nil))
                              (inner (make-ir-binding 'x nil))
                              (y (make-ir-binding 'y nil))
                              (node (analyze '(list x y z)
                                             (list inner y outer))))
                         (list (ir-show node)
                               (eq? inner
                                    (ir-reference-binding
                                     (vector-get (ir-call-arguments node)
                                                 0))))))",
                    "((:CALL LIST (:REFERENCE X 1) (:REFERENCE Y 2) "
                    "(:LOOKUP Z)) T)");
            };

            "DO bodies collapse when short"_test = [] {
                source_machine m{compiler_image()};
                m.check(
                    R"((map (fn (form) (ir-show (analyze form)))
                            '((do) (do 1) (do (f) 2))))",
                    "((:CONSTANT NIL) (:CONSTANT 1) "
                    "(:DO (:CALL F) (:CONSTANT 2)))");
            };

            "function references read the function cell"_test = [] {
                source_machine m{compiler_image()};
                m.check(
                    R"((ir-show (analyze '(call #'car (function cdr)))))",
                    "(:CALL CALL (:FUNCTION CAR) (:FUNCTION CDR))");
            };

            "macros expand during analysis"_test = [] {
                source_machine m{compiler_image()};
                m.check(
                    R"((defmacro twice (x) (list 'do x x))
                       (ir-show (analyze '(if (twice (f)) (unless a b) c))))",
                    "(:IF (:DO (:CALL F) (:CALL F)) "
                    "(:IF (:LOOKUP A) (:CONSTANT NIL) (:LOOKUP B)) "
                    "(:LOOKUP C))");
            };

            "undefined operators are analyzed as calls"_test = [] {
                source_machine m{compiler_image()};
                m.check(
                    R"((ir-show (analyze '(not-defined-yet 1 x))))",
                    "(:CALL NOT-DEFINED-YET (:CONSTANT 1) (:LOOKUP X))");
            };

            "unsupported and malformed forms escape to source"_test = [] {
                source_machine m{compiler_image()};
                m.check(
                    R"((let ((cyclic (list 'f 1 2)))
                         (set-tail! (tail (tail cyclic)) (tail cyclic))
                         (map (fn (form) (head (ir-show (analyze form))))
                              (list '(if 1 2)
                                    '(quote)
                                    '(function 1)
                                    '(%fn nil (x) x)
                                    '(let ((x 1)) x)
                                    '(f . 1)
                                    '((fn (x) x) 1)
                                    '(nil 1)
                                    cyclic))))",
                    "(:SOURCE :SOURCE :SOURCE :SOURCE :SOURCE :SOURCE :SOURCE "
                    ":SOURCE :SOURCE)");
            };

            "analyzed graphs keep their binding identity in a tape"_test = [] {
                auto from = compiler_image();
                source_machine m{std::move(from)};
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
        };
    }};

} // namespace
} // namespace wisp::test
