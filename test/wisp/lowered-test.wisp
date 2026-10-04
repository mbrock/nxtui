;; -*- mode: wisp; fill-column: 64; -*-
;;; RFC 0021's first compact lowered-code slice.

(defun condition-type (thunk)
  (try (call thunk) (catch (e k) (type-of e))))

(defun first-lowered-frame (k operation)
  (unless (top? k)
    (let ((description (%code-operation (ktx-fun k))))
      (if (and description (eq? (head description) operation)) k
        (first-lowered-frame (ktx-hop k) operation)))))

(deftest "lowering emits compact schema-native operation metadata"
  (let* ((operations (code-operations))
         (constant (head (find operations (fn (entry) (eq? (head entry) :constant)))))
         (lexical-load
          (head (find operations (fn (entry) (eq? (head entry) :lexical-load)))))
         (function-op
          (head (find operations (fn (entry) (eq? (head entry) :function)))))
         (node (lower (analyze '(let ((x 41)) (+ x 1))))))
    (expect constant)
    (expect lexical-load)
    (expect function-op)
    (expect (integer? (second constant)))
    (expect-equal (third lexical-load) '(:address :address))
    (expect-equal (code-show node)
                  '(:LET (X) ((:CONSTANT 41))
                    (:CALL + ((:LEXICAL-LOAD 0 0) (:CONSTANT 1)))))))

(deftest "lowered code remains live after dropping its IR graph and collecting"
  (let* ((graph (analyze '(let ((x 41)) (+ x 1))))
         (node (lower graph)))
    (set! graph nil)
    (gc)
    (expect-equal (eval node) 42)))

(deftest "lowered functions expose source and SET-CODE! restores source"
  (defun lower-add1 (x) (+ x 1))
  (expect (lower-function! #'lower-add1))
  (expect-equal (lower-add1 41) 42)
  (expect-equal (code #'lower-add1) '(+ x 1))
  (set-code! #'lower-add1 '(* x 3))
  (expect-equal (lower-add1 5) 15)
  (expect-equal (code #'lower-add1) '(* x 3)))

(deftest "a continuation preserves lowered lexical and argument progress"
  (defvar saved-lowered nil)
  (expect-equal
   (lowered-eval
    '(call-with-prompt 'pause
       (fn ()
         (let ((x 0))
           (list (do (set! x (+ x 1)) x)
                 (send! 'pause)
                 (do (set! x (+ x 1)) x))))
       (fn (v k) (set! saved-lowered k) 'paused)))
   'paused)
  (let* ((k (first-lowered-frame saved-lowered :call))
         (frame (code-frame k)))
    (expect k)
    (expect-equal (second frame) 1)
    (expect (eq? (third frame) #'list))
    (expect-equal (head (tail (tail (tail frame)))) '(1)))
  (expect-equal (call saved-lowered 10) '(1 10 2))
  (expect-equal (call saved-lowered 20) '(1 20 3)))

(deftest "lowered LET initializers copy progress but share outer storage"
  (defvar saved-let nil)
  (expect-equal
   (lowered-eval
    '(call-with-prompt 'pause
       (fn ()
         (let ((n 0))
           (let ((a (do (set! n (+ n 1)) n))
                 (b (send! 'pause))
                 (c (do (set! n (+ n 1)) n)))
             (list a b c n))))
       (fn (v k) (set! saved-let k) 'paused)))
   'paused)
  (let ((frame (code-frame (first-lowered-frame saved-let :let))))
    (expect-equal (second frame) 1)
    (expect-equal (head (tail (tail (tail frame)))) '(1)))
  (expect-equal (call saved-let 10) '(1 10 2 2))
  (expect-equal (call saved-let 20) '(1 20 3 3)))

(deftest "lowered callees are retained before arguments redefine them"
  (defun lowered-target (x) (list 'old x))
  (expect-equal
   (lowered-eval
    '(list (lowered-target
            (do (defun lowered-target (x) (list 'new x)) 1))
           (lowered-target 2)))
   '((old 1) (new 2))))

(deftest "a lowered callee stays resolved across suspension and redefinition"
  (defvar saved-callee nil)
  (defun target (a b) (list 'old a b))
  (lowered-eval
   '(call-with-prompt 'pause
      (fn () (target 'first (send! 'pause)))
      (fn (v k) (set! saved-callee k) 'paused)))
  (defun target (a b) (list 'new a b))
  (expect-equal (call saved-callee 10) '(old first 10))
  (expect-equal (target 'first 20) '(new first 20)))

(deftest "ordinary vectors remain self-evaluating and quoted objects keep identity"
  (let ((value (vector 256 99)))
    (expect (eq? (lowered-eval value) value))
    (let ((node (lower (analyze (list 'quote value)))))
      (vector-set! value 1 42)
      (expect (eq? (eval node) value))
      (expect-equal (vector-get (eval node) 1) 42))))

(deftest "lowering retains macro expansion at function definition"
  (defmacro lowered-early () 1)
  (defun lowered-uses-early () (lowered-early))
  (lower-function! #'lowered-uses-early)
  (defmacro lowered-early () 2)
  (expect-equal (lowered-uses-early) 1))

(deftest "lowered lexical writes use explicit lexical addresses"
  (expect-equal
   (lowered-eval
    '(let ((x 1))
       (let ((f (fn (n) (set! x (+ x n)))))
         (call f 4)
         (call f 7)
         x)))
   12))

(deftest "lowered tail recursion keeps a flat control stack"
  (defun lowered-frame-count (k)
    (if (top? k) 0 (+ 1 (lowered-frame-count (ktx-hop k)))))
  (defun lowered-frames-here () (lowered-frame-count (get/cc)))
  (defun lowered-count-down (n)
    (if (eq? n 0)
        (lowered-frames-here)
      (lowered-count-down (- n 1))))
  (lower-function! #'lowered-count-down)
  (expect-equal (lowered-count-down 3) (lowered-count-down 3000)))

(deftest "lowered and source functions call each other"
  (defun lower-cross-even? (n)
    (if (eq? n 0) t (lower-cross-odd? (- n 1))))
  (lower-function! #'lower-cross-even?)
  (defun lower-cross-odd? (n)
    (if (eq? n 0) nil (lower-cross-even? (- n 1))))
  (expect-equal (lower-cross-even? 10) t)
  (expect-equal (lower-cross-even? 7) nil)
  (expect-equal (lower-cross-odd? 7) t)
  (prepare-function! #'lower-cross-odd?)
  (expect-equal (lower-cross-even? 7) nil)
  (expect-equal (lower-cross-odd? 7) t))

(deftest "unsupported special forms lower through the source operation"
  (let* ((source-op
          (head (find (code-operations)
                      (fn (entry) (eq? (head entry) :source)))))
         (node (lower (analyze '(if t 42)))))
    ;; The malformed IF is deliberately left to source evaluation.
    (expect-equal (record-type node) (second source-op))
    (expect-equal (head (code-show node)) :source)))

(deftest "invalid explicit lexical addresses signal conditions"
  (let* ((node (lower (analyze '(let ((x 42)) x))))
         (load (record-get node 2)))
    (record-set! load 0 20)
    (expect-equal (condition-type (fn () (eval node)))
                  'invalid-expression)))

(deftest "malformed lowered operations reject bad opcodes arities and operands"
  (let* ((operations (code-operations))
         (constant (head (find operations (fn (entry) (eq? (head entry) :constant)))))
         (lexical-load
          (head (find operations (fn (entry) (eq? (head entry) :lexical-load))))))
    ;; Pick an opcode outside the schema's operation table.
    (expect-equal
     (condition-type (fn () (eval (record 999999))))
     'invalid-expression)
    ;; CONSTANT takes one operand; a missing operand is malformed.
    (expect-equal
     (condition-type (fn () (eval (record (second constant)))))
     'invalid-expression)
    ;; LEXICAL-LOAD takes two fixnum addresses, not a symbol operand.
    (expect-equal
     (condition-type
      (fn () (eval (record (second lexical-load) 'not-an-address 0))))
     'invalid-expression)))

(deftest "lowering rejects unchecked IR and execution rejects source children"
  (expect-equal
   (condition-type (fn () (lower (make-ir-sequence 5)))) 'invalid-ir)
  (let ((node (lower (analyze '(+ 1 2)))))
    (vector-set! (record-get node 1) 1 (make-ir-constant 2))
    (expect-equal (condition-type (fn () (eval node))) 'invalid-expression)))

(deftest "lower-package! bootstraps and lowers the compiler twice" :slow
  (let ((package (find-package "WISP")))
    (expect (> (lower-package! package) 200))
    (expect-equal (map (fn (x) (* x x)) '(1 2 3)) '(1 4 9))
    (expect-equal (lowered-eval '(let ((x 2)) (* x 21))) 42)
    (expect (> (lower-package! package) 200))
    (expect-equal (lowered-eval '(let ((x 3)) (+ x 4))) 7)))
