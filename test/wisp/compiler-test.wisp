;; -*- mode: wisp; fill-column: 64; -*-
;;; Analysis into semantic records (RFC 0020, src/wisp/compiler.wisp).

(defun show (form &optional scope)
  (ir-show (analyze form scope)))

(defun argument (call-node i)
  (vector-get (ir-call-arguments call-node) i))

(deftest "IR nodes are DEFSTRUCT records"
  (expect-equal (type-of (analyze 1)) 'ir-constant)
  (expect-equal (type-of (analyze 'x)) 'ir-lookup)
  (expect (ir-constant? (analyze "s"))))

(deftest "self-evaluating atoms and quotations become constants"
  (expect-equal
   (show '(list 1 "two" :three nil t 'four '(5)))
   '(:call list (:constant 1) (:constant "two")
     (:constant :three) (:constant nil) (:constant t)
     (:constant four) (:constant (5)))))

(deftest "both uses of a binding share one binding object"
  (let* ((x (make-ir-binding 'x nil))
         (node (analyze '(if x (foo x) 17) (list x))))
    (expect-equal
     (ir-show node)
     '(:if (:reference x 1)
           (:call foo (:reference x 1))
           (:constant 17)))
    (expect (eq? x (ir-reference-binding (ir-branch-test node))))
    (expect (eq? x (ir-reference-binding
                    (argument (ir-branch-consequent node) 0))))))

(deftest "the innermost binding of a name wins"
  (let* ((outer (make-ir-binding 'x nil))
         (inner (make-ir-binding 'x nil))
         (y (make-ir-binding 'y nil))
         (node (analyze '(list x y z) (list inner y outer))))
    (expect-equal
     (ir-show node)
     '(:call list (:reference x 1) (:reference y 2) (:lookup z)))
    (expect (eq? inner (ir-reference-binding (argument node 0))))))

(deftest "DO bodies collapse when short"
  (expect-equal (show '(do)) '(:constant nil))
  (expect-equal (show '(do 1)) '(:constant 1))
  (expect-equal (show '(do (f) 2))
                '(:do (:call f) (:constant 2))))

(deftest "function references read the function cell"
  (expect-equal (show '(call #'car (function cdr)))
                '(:call call (:function car) (:function cdr))))

(deftest "macros expand during analysis"
  (defmacro twice (x) (list 'do x x))
  (expect-equal
   (show '(if (twice (f)) (unless a b) c))
   '(:if (:do (:call f) (:call f))
         (:if (:lookup a) (:constant nil) (:lookup b))
         (:lookup c))))

(deftest "undefined operators are analyzed as calls"
  (expect-equal (show '(not-defined-yet 1 x))
                '(:call not-defined-yet (:constant 1) (:lookup x))))

(deftest "unsupported and malformed forms escape to source"
  (let ((cyclic (list 'f 1 2)))
    (set-tail! (tail (tail cyclic)) (tail cyclic))
    (for-each (list '(if 1 2)
                    '(quote)
                    '(function 1)
                    '(%fn nil (x) x)
                    '(let ((x 1)) x)
                    '(f . 1)
                    '((fn (x) x) 1)
                    '(nil 1)
                    cyclic)
              (fn (form)
                (expect (ir-source? (analyze form)))))))
