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
                    '(%macro-fn (x) x)
                    '(f . 1)
                    '((fn (x) x) 1)
                    '(nil 1)
                    cyclic)
              (fn (form)
                (expect (ir-source? (analyze form)))))))

;;; Binders

(defun binding-of (node)
  (ir-reference-binding node))

(deftest "a function literal binds its parameter once"
  (let* ((closure (analyze '(fn (x) (if x (foo x) 17))))
         (function (ir-closure-function closure))
         (x (vector-get (ir-function-bindings function) 0))
         (body (ir-function-body function)))
    (expect-equal
     (ir-show closure)
     '(:fn nil ((x 1))
           (:if (:reference x 1)
                (:call foo (:reference x 1))
                (:constant 17))))
    (expect (eq? function (ir-binding-owner x)))
    (expect (eq? x (binding-of (ir-branch-test body))))
    (expect (eq? x (binding-of
                    (argument (ir-branch-consequent body) 0))))))

(deftest "parameters keep their required, optional, and rest structure"
  (let* ((function (ir-closure-function
                    (analyze '(%fn f (a &optional b c &rest ds)
                                (list a b c ds)))))
         (parameters (ir-function-parameters function)))
    (expect-equal (vector-length (ir-parameters-required parameters)) 1)
    (expect-equal (vector-length (ir-parameters-optional parameters)) 2)
    (expect-equal (ir-binding-name (ir-parameters-rest parameters)) 'ds)
    (expect-equal (ir-parameters-source parameters)
                  '(a &optional b c &rest ds))
    (expect-equal
     (ir-show (analyze '(%fn f (a &optional b c &rest ds)
                          (list a b c ds))))
     '(:fn f ((a 1) &optional (b 2) (c 3) &rest (ds 4))
           (:call list (:reference a 1) (:reference b 2)
                  (:reference c 3) (:reference ds 4)))))
  (expect-equal (ir-show (analyze '(%fn nil (&body xs) xs)))
                '(:fn nil (&rest (xs 1)) (:reference xs 1))))

(deftest "a duplicated parameter resolves to its first occurrence"
  (let* ((function (ir-closure-function (analyze '(fn (x x) x))))
         (bindings (ir-function-bindings function)))
    (expect (eq? (vector-get bindings 0)
                 (binding-of (ir-function-body function))))))

(deftest "malformed function literals escape to source"
  (for-each (list '(%fn nil (1) x)
                  '(%fn nil (:key) x)
                  '(%fn nil (&rest) x)
                  '(%fn nil (&rest a b) x)
                  '(%fn nil (a . b) x)
                  '(%fn 1 (x) x)
                  '(%fn nil (x)))
            (fn (form) (expect (ir-source? (analyze form))))))

(deftest "LET initializers see the enclosing scope"
  (expect-equal
   (show '(let ((x 1)) (let ((x 2) (y x)) (list x y))))
   '(:let (((x 1) (:constant 1)))
          (:let (((x 2) (:constant 2))
                 ((y 3) (:reference x 1)))
                (:call list (:reference x 2) (:reference y 3))))))

(deftest "a duplicated LET name resolves to its last clause"
  (let ((node (analyze '(let ((x 1) (x 2)) x))))
    (expect (eq? (vector-get (ir-let-bindings node) 1)
                 (binding-of (ir-let-body node))))
    (expect (eq? node (ir-binding-owner
                       (binding-of (ir-let-body node)))))))

(deftest "LET without clauses is its body"
  (expect-equal (show '(let () 1 2))
                '(:do (:constant 1) (:constant 2)))
  (expect-equal (show '(let ())) '(:constant nil)))

(deftest "malformed LET forms escape to source"
  (for-each (list '(let)
                  '(let x)
                  '(let ((x)) x)
                  '(let ((1 2)) 3)
                  '(let ((x 1 2)) x)
                  '(let (x) x))
            (fn (form) (expect (ir-source? (analyze form))))))

(deftest "assignment targets the lexical binding or the global"
  (expect-equal
   (show '(let ((n 0)) (set! n (+ n 1)) (set! g n)))
   '(:let (((n 1) (:constant 0)))
          (:do (:set (:reference n 1)
                     (:call + (:reference n 1) (:constant 1)))
               (:set (:lookup g) (:reference n 1)))))
  (expect-equal (show '(%set! name value))
                '(:call %set! (:lookup name) (:lookup value))))

(deftest "DEFUN analyzes to a named closure stored in a function cell"
  (expect-equal
   (show '(defun add1 (n) (+ n 1)))
   '(:call set-symbol-function! (:constant add1)
           (:fn add1 ((n 1))
                (:call + (:reference n 1) (:constant 1))))))

;;; Captures

(defun capture-names (form)
  (map #'ir-binding-name (ir-captures (find-closure (analyze form)))))

;; The first closure in a LET body or sequence, for these tests.
(defun find-closure (node)
  (cond ((ir-closure? node) node)
        ((ir-let? node) (find-closure (ir-let-body node)))
        ((ir-sequence? node)
         (find-closure (vector-get (ir-sequence-forms node) 0)))
        (t nil)))

(deftest "a closure captures the outer bindings it uses"
  (expect-equal (capture-names '(let ((x 1) (y 2)) (fn (z) (list x z))))
                '(x))
  (expect-equal
   (show '(let ((x 1)) (fn (z) (list x z))))
   '(:let (((x 1) (:constant 1)))
          (:fn nil ((z 2)) :captures ((x 1))
               (:call list (:reference x 1) (:reference z 2))))))

(deftest "captures pass through intervening functions"
  (let* ((outer (find-closure
                 (analyze '(let ((x 1)) (fn () (fn () (set! x 5)))))))
         (inner (ir-function-body (ir-closure-function outer))))
    (expect-equal (map #'ir-binding-name (ir-captures outer)) '(x))
    (expect-equal (map #'ir-binding-name (ir-captures inner)) '(x))
    (expect (eq? (head (ir-captures outer))
                 (head (ir-captures inner))))))

(deftest "bindings introduced inside a function are not its captures"
  (let* ((outer (analyze '(fn () (let ((a 1)) (fn () a)))))
         (inner (find-closure (ir-function-body
                               (ir-closure-function outer)))))
    (expect-equal (ir-captures outer) nil)
    (expect-equal (map #'ir-binding-name (ir-captures inner)) '(a))))

(deftest "a source escape captures every visible binding"
  (expect-equal
   (capture-names '(let ((x 1) (y 2)) (fn (z) (%macro-fn () x))))
   '(y x)))
