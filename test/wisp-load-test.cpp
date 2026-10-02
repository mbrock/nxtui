#include <wisp/load.hpp>
#include <wisp/printer.hpp>

#include "test.hpp"
#include "wisp-base.hpp"

namespace wisp::test {
namespace {

using namespace boost::ut;
using namespace std::chrono_literals;

struct source_machine
{
    explicit source_machine(std::unique_ptr<image> from = image::fresh())
        : owner(std::move(from))
    {
    }

    std::unique_ptr<image> owner;
    heap & h = owner->storage;
    evaluator & vm = owner->machine;
    root last{h};

    word load(std::string_view text, std::size_t quantum = 257)
    {
        loader source{h, vm, text};
        for (std::size_t turns = 0; turns < 40000; ++turns) {
            const auto state = source.advance(quantum);
            h.collect();
            if (state == evaluation::failed)
                throw std::runtime_error(
                    "Wisp source failed at byte "
                    + std::to_string(source.position()) + ": "
                    + print(h, h.get<tag::run, field::err>(source.run())));
            if (state == evaluation::done) {
                last.set(source.run());
                return last.get() == nil
                           ? nil
                           : h.get<tag::run, field::val>(last.get());
            }
        }
        throw std::runtime_error("Wisp source exhausted its test budget");
    }

    void check(std::string_view source, std::string_view expected)
    {
        expect(print(h, load(source)) == expected);
    }
};

static suite source_tests{
    "WISP SOURCE", [] {
        "loading pauses, roots its run, and distinguishes empty input from NIL"_test =
            [] {
                source_machine m;
                loader source{
                    m.h, m.vm, "(set-symbol-value! 'x 19) (+ x 23)"};
                expect(source.advance(0) == evaluation::runnable);
                expect(source.run() == nil && source.position() == 0u);
                expect(source.advance(1) == evaluation::runnable);
                expect(source.run() != nil);
                auto state = evaluation::runnable;
                for (unsigned i = 0;
                     i < 200 && state == evaluation::runnable;
                     ++i) {
                    m.h.collect();
                    state = source.advance(1);
                }
                expect(state == evaluation::done);
                expect(
                    (m.h.get<tag::run, field::val>(source.run()) == 42u));
                auto row = m.h.read<tag::run>(source.run());
                expect(source.advance(100) == evaluation::done);
                expect(m.h.read<tag::run>(source.run()) == row);
                loader empty{m.h, m.vm, " ; comment\n"};
                expect(
                    empty.advance(1) == evaluation::done
                    && empty.run() == nil);
                loader nil_form{m.h, m.vm, "nil"};
                expect(nil_form.advance(10) == evaluation::done);
                expect(nil_form.run() != nil);
                expect(
                    (m.h.get<tag::run, field::val>(nil_form.run()) == nil));
            };

        "parse and evaluation errors terminate loading before later side effects"_test =
            [] {
                for (auto suffix :
                     {"( (set-symbol-value! 'x 99)",
                      "(head 1) (set-symbol-value! 'x 99)"}) {
                    source_machine m;
                    loader source{
                        m.h,
                        m.vm,
                        std::string("(set-symbol-value! 'x 7) ") + suffix};
                    auto state = evaluation::runnable;
                    for (unsigned i = 0;
                         i < 100 && state == evaluation::runnable;
                         ++i) {
                        state = source.advance(1);
                        m.h.collect();
                    }
                    expect(state == evaluation::failed);
                    expect(
                        (m.h.get<tag::sym, field::val>(m.vm.intern("X"))
                         == 7u));
                    expect(
                        (m.h.get<tag::run, field::err>(source.run())
                         != nil));
                    auto row = m.h.read<tag::run>(source.run());
                    expect(source.advance(1000) == evaluation::failed);
                    expect(m.h.read<tag::run>(source.run()) == row);
                }
            };

        "the guest bootstrap supplies definitions, quasiquotation, and eager lambdas"_test
            .with_timeout(10s) = [] {
            source_machine m{base_image()};
            m.check(
                R"(
            (defun gather (x &optional y &rest zs) (list x y zs))
            (defmacro twice (x) `(+ ,x ,x))
            (list (gather 1 9 2 3)
                  (twice 7)
                  `(a ,(+ 2 3) ,@(map (fn (n) (* n 10)) '(2 4))))
        )",
                "((1 9 (2 3)) 14 (A 5 20 40))");
            m.check(
                R"(
            (defmacro add-ten (x) (list '+ x 10))
            (let ((f (let ((offset 1)) (fn (x) (add-ten (+ offset x))))))
              (let ((before (function-call-count #'add-ten)))
                (list (list (call f 0) (call f 1))
                      (eq? before (function-call-count #'add-ten)))))
        )",
                "((11 12) T)");
            m.check(
                R"(
            (macroexpand-completely
              '(future-callee (fn (x &optional y &rest zs) (when x (list y zs)))))
        )",
                "(FUTURE-CALLEE (%FN NIL (X &OPTIONAL Y &REST ZS) (IF X (LIST Y ZS) NIL)))");
            m.check(
                R"(
            (defmacro branch (x)
              (list 'if x (list 'list (list 'branch x) (list 'branch x)) 7))
            (let ((before (function-call-count #'branch)))
              (let ((f (call-with-binding '*macroexpand-limit* 8
                         (%fn nil () (fn (x) (branch x))))))
                (list (call f nil)
                      (eq? 8 (- (function-call-count #'branch) before))
                      *macroexpand-budget*)))
        )",
                "(7 T NIL)");
            m.check(
                R"(
            (defun live-target (x) (+ x 1))
            (defun live-caller (x) (live-target x))
            (let ((old #'live-target) (saved nil))
              (let ((initial
                      (call-with-prompt 'save-call
                        (fn () (live-target (send! 'save-call 'paused)))
                        (fn (v k) (do (set! saved k) v)))))
                (defun live-target (x) (+ x 100))
                (list initial (live-caller 2) (call old 2)
                      (call saved 5) (call saved 9)
                      (live-target
                        (do (set-symbol-function! 'live-target (fn (x) (+ x 1000)))
                            3))
                      (live-caller 3))))
        )",
                "(PAUSED 102 3 6 10 103 1003)");
        };

        "deep effects resume and raise through the guest library across collection"_test
            .with_timeout(10s) = [] {
            source_machine m{base_image()};
            m.check(
                R"(
            (call-with-effect-handler 'ask
              (fn () (+ (send! 'ask 2) (send! 'ask 3)))
              (fn (request resume raise) (call resume (* request 10))))
        )",
                "50");
            // In the reference this exposes the missing UNHANDLED-ERROR
            // fallback. The port re-signals at the caller when no
            // target ERROR prompt occurs inside the captured slice.
            m.check(
                R"(
            (try
              (call-with-effect-handler 'ask
                (fn () (send! 'ask nil))
                (fn (request resume raise) (call raise 'nope)))
              (catch (error restart) (list 'caught (head error))))
        )",
                "(CAUGHT NOPE)");
            // Here ERROR is inside the captured slice, not at the
            // caller.
            m.check(
                R"(
            (call-with-effect-handler 'ask
              (fn ()
                (try (+ 19 (send! 'ask nil))
                  (catch (error restart) (list 'inside (head error)))))
              (fn (request resume raise) (call raise 'remote)))
        )",
                "(INSIDE REMOTE)");
            m.check(
                R"(
            (defvar saved-resume nil)
            (call-with-effect-handler 'widget
              (fn () (do (send! 'widget 'first) (send! 'widget 'second)))
              (fn (view resume raise) (set! saved-resume resume) view))
        )",
                "FIRST");
            m.h.collect();
            m.check("(call saved-resume 'again)", "SECOND");
        };

        "strings, source streams, and stream effects compose without native I/O"_test
            .with_timeout(10s) = [] {
            source_machine m{base_image()};
            m.check(
                R"(
            (let ((input (string-input-stream "nil 42 :ready"))
                  (eof (fn () 'end)))
              (list (read input eof) (read input eof)
                    (read input eof) (read input eof)))
        )",
                "(NIL 42 :READY END)");
            m.check(
                R"(
            (list (read-many-from-string "nil (1 . 2) :ok")
                  (read-from-string "42 99")
                  (equal? '(a "quoted\" and \\ slash" 7)
                          (read-from-string (print-to-string '(a "quoted\" and \\ slash" 7)))))
        )",
                "((NIL (1 . 2) :OK) 42 T)");
            m.check(
                R"(
            (call-with-effect-handler 'capture
              (fn ()
                (binding ((*standard-output* 'capture))
                  (write "hello ") (print 'world) ""))
              (fn (request resume raise)
                (string-append (apply #'string-append (tail request))
                               (call resume nil))))
        )",
                "\"hello WORLD\\n\"");
            m.check(
                R"(
            (call-with-effect-handler 'capture
              (fn ()
                (binding ((*standard-output* 'capture))
                  (eval '(do (write "eval ") (print 'world) ""))))
              (fn (request resume raise)
                (string-append (apply #'string-append (tail request))
                               (call resume nil))))
        )",
                "\"eval WORLD\\n\"");
            m.check(
                R"(
            (try (eval '(error 'eval-failure))
              (catch (condition restart) (head condition)))
        )",
                "EVAL-FAILURE");
            m.check(
                R"(
            (try (write "not silently discarded")
              (catch (error restart) (list (head error) (second error))))
        )",
                "(HOST-IO-UNAVAILABLE :STDOUT)");
        };
    }};

} // namespace
} // namespace wisp::test
