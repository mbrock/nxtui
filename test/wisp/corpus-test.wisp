;; -*- mode: wisp; fill-column: 64; -*-
;;; Semantic corpus for source, prepared IR, and lowered execution.
;;
;; Each case is a small program, a list of top-level forms, with
;; the value of its last form under source interpretation. Every
;; test runs in its own machine, so multi-shot mutation cannot
;; leak between cases. Each execution mode runs the same cases
;; under its own contract.
;;
;; Where RFC 0020's liveness contract deliberately makes prepared
;; code differ, a case also gives the prepared result and the
;; reason, which makes this file the mode/behavior matrix:
;;
;;   (defcase NAME PROGRAM SOURCE-RESULT
;;     [PREPARED-RESULT REASON])

;; Cases in declaration order: (name program source prepared reason).
(defvar *corpus* nil)

;; Each case is a test in source, prepared, and lowered modes.
(defmacro defcase (name program source &optional prepared reason)
  (when (not (eq? (nil? prepared) (nil? reason)))
    (error 'corpus-case-needs-reason name))
  `(do (set! *corpus*
             (append *corpus* (list (list ,name ',program ',source
                                          ',prepared ,reason))))
       (deftest ,name
         (expect-equal (run-forms ',program) ',source))
       (deftest ,(string-append name " [prepared]")
         (expect-equal (run-prepared-forms ',program)
                       ',(if reason prepared source)))
       (deftest ,(string-append name " [lowered]")
         (expect-equal (run-lowered-forms ',program)
                       ',(if reason prepared source)))))

(defun run-prepared-forms (forms)
  (if (nil? (tail forms))
      (prepared-eval (head forms))
    (do (prepared-eval (head forms))
        (run-prepared-forms (tail forms)))))

(defun run-lowered-forms (forms)
  (if (nil? (tail forms))
      (lowered-without-descriptors (head forms))
    (do (lowered-without-descriptors (head forms))
        (run-lowered-forms (tail forms)))))

;; After lowering, disable every IR descriptor's name. Even an accidental
;; call to the old record decoder cannot recognize an IR type. Restore the
;; names before analyzing the next top-level form (macros are live there).
(defun lowered-without-descriptors (form)
  (let* ((node (lower (analyze form)))
         (descriptors (list <ir-binding> <ir-reference> <ir-lookup>
                            <ir-function-reference> <ir-constant>
                            <ir-assignment> <ir-call> <ir-branch>
                            <ir-sequence> <ir-let> <ir-function>
                            <ir-parameters> <ir-closure> <ir-source>))
         (names (map (fn (descriptor) (record-get descriptor 0)) descriptors)))
    (for-each descriptors (fn (descriptor) (record-set! descriptor 0 nil)))
    (let ((result (eval node)))
      (for-each (%ir-zip descriptors names)
                (fn (pair) (record-set! (head pair) 0 (tail pair))))
      result)))

;;; Values and evaluation order

(defcase "constants and quotation evaluate to themselves"
  ((list 1 "two" 'three nil t :key '(4 5)))
  (1 "two" three nil t :key (4 5)))

(defcase "arguments are evaluated left to right"
  ((let ((log nil))
     (list (do (set! log (cons 1 log)) 'a)
           (do (set! log (cons 2 log)) 'b))
     (reverse log)))
  (1 2))

(defcase "value and function namespaces are distinct"
  ((defun x () 'function)
   (let ((x 'value)) (list x (x) (call #'x))))
  (value function function))

;;; Lexical binding

(defcase "inner bindings shadow outer ones only in their body"
  ((let ((x 1)) (list x (let ((x 2)) x) x)))
  (1 2 1))

(defcase "LET initializers see the enclosing scope"
  ((let ((x 1)) (let ((x 2) (y x)) (list x y))))
  (2 1))

(defcase "duplicate names in one LET resolve to the last"
  ((let ((x 1) (x 2)) x))
  2)

(defcase "duplicate parameter names resolve to the first"
  ((call (fn (x x) x) 1 2))
  1)

(defcase "optional parameters default to NIL and rest collects a list"
  ((defun f (a &optional b &rest cs) (list a b cs))
   (list (f 1) (f 1 2) (f 1 2 3 4)))
  ((1 nil nil) (1 2 nil) (1 2 (3 4))))

(defcase "missing required arguments signal a program error"
  ((defun needs-one (a) a)
   (try (needs-one) (catch (e k) (type-of e))))
  program-error)

;;; Closures and shared locations

(defcase "closures share one mutable location"
  ((let ((n 0))
     (let ((inc (fn () (set! n (+ n 1))))
           (get (fn () n)))
       (call inc)
       (call inc)
       (call get))))
  2)

(defcase "each closure creation captures a fresh location"
  ((defun make-counter ()
     (let ((n 0)) (fn () (set! n (+ n 1)))))
   (let ((a (make-counter)) (b (make-counter)))
     (call a) (call a)
     (list (call a) (call b))))
  (3 1))

(defcase "assignment reaches through intervening closures"
  ((let ((x 1))
     (call (fn () (call (fn () (set! x 5)))))
     x))
  5)

;;; Dynamic scope

(defcase "dynamic bindings are visible to callees and restored after"
  ((defparameter *depth* 1)
   (defun read-depth () *depth*)
   (list (read-depth)
         (binding ((*depth* 2)) (read-depth))
         (read-depth)))
  (1 2 1))

(defcase "explicit lexical binders stay lexical after a dynamic declaration"
  ((defvar y 1)
   (let ((f (let ((y 10)) (fn () y))))
     (set-symbol-dynamic! 'y t)
     (call f)))
  10)

;;; Redefinition, following the liveness contract

(defcase "the callee is resolved before its arguments run"
  ((defun target (x) (list 'old x))
   (list (target (do (defun target (x) (list 'new x)) 1))
         (target 2)))
  ((old 1) (new 2)))

(defcase "redefining a function updates existing callers"
  ((defun callee () 1)
   (defun caller () (callee))
   (defun callee () 2)
   (caller))
  2)

(defcase "DEFUN bodies keep the macro expansion from definition time"
  ((defmacro early () 1)
   (defun uses-early () (early))
   (defmacro early () 2)
   (uses-early))
  1)

(defcase "unexpanded bodies see a redefined macro"
  ((defmacro late () 1)
   (set-symbol-function! 'uses-late (%fn uses-late () (late)))
   (defmacro late () 2)
   (uses-late))
  2
  1 "preparation expands macros once")

(defcase "a function that becomes a macro during its arguments is still called"
  ((defun shape (x) (list 'function x))
   (shape (do (defmacro shape (x) (list 'quote (list 'macro x))) 1)))
  (function 1))

(defcase "a caller meets a function that has become a macro"
  ((defun shifty (x) (list 'function x))
   (set-symbol-function! 'calls-shifty
                         (%fn calls-shifty () (shifty 5)))
   (defmacro shifty (x) (list 'quote (list 'macro x)))
   (try (calls-shifty) (catch (e k) (type-of e))))
  (macro 5)
  invalid-function "a prepared call site signals instead of expanding")

(defcase "editing a function's source conses changes its behavior"
  ((defun edit-me () (+ 1 2))
   (set-head! (tail (code #'edit-me)) 10)
   (edit-me))
  12
  3 "prepared code snapshots its syntax")

;;; Reflection and evaluation scope

(defcase "ENV returns the actual lexical store"
  ((let ((x 1)) (eq? (env) (env))))
  t)

(defcase "assignment through ENV is visible to the code"
  ((let ((x 1))
     (vector-set! (head (env)) 1 42)
     x))
  42)

(defcase "public EVAL excludes caller locals"
  ((defvar z 'global)
   (let ((z 'local)) (eval 'z)))
  global)

(defcase "macro expansions see call-site locals"
  ((defmacro get-x () 'x)
   (let ((x 7)) (get-x)))
  7)

;;; Conditions

(defcase "primitive type failures signal conditions"
  ((try (+ 1 'a)
     (catch (e k) (list (type-of e) (type-of (record-get e 1))))))
  (builtin-failure type-mismatch))

(defcase "fixnum overflow signals a condition"
  ((try (* 1073741823 2)
     (catch (e k) (list (type-of e) (type-of (record-get e 1))))))
  (builtin-failure fixnum-overflow))

;;; Control

(defcase "deep self tail recursion completes"
  ((defun count-down (n)
     (if (eq? n 0) 'done (count-down (- n 1))))
   (count-down 20000))
  done)

(defcase "a call suspended between arguments resumes twice"
  ((defvar saved nil)
   (defvar initial
     (call-with-prompt 'pause
       (fn ()
         (let ((x 0))
           (list (do (set! x (+ x 1)) x)
                 (send! 'pause)
                 (do (set! x (+ x 1)) x))))
       (fn (v k) (set! saved k) 'paused)))
   (list initial (call saved 10) (call saved 20)))
  (paused (1 10 2) (1 20 3)))

(defcase "resumptions share lexical locations but not dynamic bindings"
  ((defparameter *p* 0)
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
   (list initial (call again nil) (call again nil) *p*))
  (grabbed (1 2) (2 2) 0))

;;; Analysis coverage

(defun %tree-includes? (tree x)
  (cond ((eq? tree x) t)
        ((pair? tree) (or (%tree-includes? (head tree) x)
                          (%tree-includes? (tail tree) x)))
        (t nil)))

(deftest "every corpus form analyzes into a well-formed graph"
  (for-each *corpus*
    (fn (case)
      (for-each (second case)
        (fn (form)
          (let ((node (analyze form)))
            (expect (not (%tree-includes? (ir-show node) :unknown)))
            (expect-equal (ir-check node) nil)))))))
