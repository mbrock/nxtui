;; -*- mode: wisp; fill-column: 64; -*-
;;; Prepared execution: the evaluator running IR records directly
;;; (RFC 0020). The corpus checks results in both modes; these
;;; tests check that prepared code really runs as IR, and how it
;;; meets source code, continuations, and malformed graphs.

;; The types of the callee fields of a continuation's frames,
;; innermost first, up to LIMIT frames.
(defun frame-types (k limit)
  (if (or (top? k) (eq? limit 0)) nil
    (cons (type-of (ktx-fun k))
          (frame-types (ktx-hop k) (- limit 1)))))

;; The first frame of K whose callee field is an IR-CALL.
(defun first-ir-call-frame (k)
  (cond ((top? k) nil)
        ((ir-call? (ktx-fun k)) k)
        (t (first-ir-call-frame (ktx-hop k)))))

(defun condition-type (thunk)
  (try (call thunk) (catch (e k) (type-of e))))

(deftest "a prepared closure runs IR and shows its source as CODE"
  (prepared-eval '(defun add1 (n) (+ n 1)))
  (expect-equal (add1 41) 42)
  (expect-equal (function-call-count #'add1) 1)
  (expect-equal (code #'add1) '(+ n 1)))

(deftest "SET-CODE! returns a prepared closure to source"
  (prepared-eval '(defun twice (n) (* n 2)))
  (set-code! #'twice '(* n 3))
  (expect-equal (twice 5) 15)
  (expect-equal (code #'twice) '(* n 3)))

(deftest "a suspended prepared call keeps its argument progress"
  (defvar saved nil)
  (prepared-eval
   '(call-with-prompt 'pause
      (fn ()
        (let ((x 0))
          (list (do (set! x (+ x 1)) x)
                (send! 'pause)
                (do (set! x (+ x 1)) x))))
      (fn (v k) (set! saved k) 'paused)))
  (let ((frame (first-ir-call-frame saved)))
    (expect frame)
    (expect-equal (ktx-arg frame) 1)
    (expect (eq? (vector-get (ktx-acc frame) 0) #'list))
    (expect-equal (vector-get (ktx-acc frame) 1) 1))
  (expect-equal (call saved 10) '(1 10 2))
  (expect-equal (call saved 20) '(1 20 3)))

(deftest "prepared tail calls keep a flat control stack"
  (defun frames-here () (frame-types (get/cc) 100))
  (prepared-eval
   '(defun probe (n)
      (if (eq? n 0) (frames-here) (probe (- n 1)))))
  (expect-equal (probe 3) (probe 3000)))

(deftest "prepared and source functions call each other"
  (prepared-eval
   '(defun prepared-even? (n)
      (if (eq? n 0) t (source-odd? (- n 1)))))
  (defun source-odd? (n)
    (if (eq? n 0) nil (prepared-even? (- n 1))))
  (expect-equal (prepared-even? 10) t)
  (expect-equal (prepared-even? 7) nil)
  (expect-equal (source-odd? 7) t))

(deftest "mixed tail calls stay flat in both directions"
  (defun frames-here () (frame-types (get/cc) 100))
  (prepared-eval
   '(defun prepared-down (n)
      (if (eq? n 0) (frames-here) (source-down (- n 1)))))
  (defun source-down (n)
    (if (eq? n 0) (frames-here) (prepared-down (- n 1))))
  (expect-equal (prepared-down 4) (prepared-down 4000)))

(defun count-of (x xs)
  (length (filter xs (fn (y) (eq? x y)))))

(deftest "each pending prepared call keeps one IR frame"
  (defun frames-here () (frame-types (get/cc) 100))
  (prepared-eval
   '(defun nest (n)
      (if (eq? n 0) (frames-here) (head (list (nest (- n 1)))))))
  ;; Each level waits in two calls, HEAD and LIST.
  (expect-equal (- (count-of 'ir-call (nest 5))
                   (count-of 'ir-call (nest 2)))
                6))

(deftest "assignment reaches globals and closures"
  (defvar *counter* 0)
  (prepared-eval '(set! *counter* (+ *counter* 5)))
  (expect-equal *counter* 5)
  (expect-equal
   (prepared-eval
    '(let ((n 0))
       (let ((bump (fn (by) (set! n (+ n by)))))
         (call bump 2)
         (call bump 3)
         n)))
   5))

(deftest "a source escape runs in the prepared scope"
  (let* ((node (analyze '(let ((x 41)) x)))
         (x (vector-get (ir-let-bindings node) 0)))
    (set-ir-let-body! node (make-ir-source '(+ x 1) (list x)))
    (expect (ir-valid? node))
    (expect-equal (eval node) 42)))

(deftest "a prepared LET binds duplicate names like source LET"
  (expect-equal (prepared-eval '(let ((x 1) (x 2)) x)) 2)
  (expect-equal (prepared-eval '(call (fn (x x) x) 1 2)) 1))

(deftest "malformed IR signals conditions"
  (expect-equal (condition-type (fn () (eval (make-ir-sequence 5))))
                'invalid-expression)
  (expect-equal (condition-type
                 (fn () (eval (make-ir-call (make-ir-constant 1)
                                            (vector)))))
                'invalid-expression)
  (expect-equal (condition-type
                 (fn () (eval (make-ir-call
                               (make-ir-function-reference 'undefined-f)
                               (vector)))))
                'undefined-function)
  (expect-equal (condition-type
                 (fn () (eval (make-ir-reference 'not-a-binding))))
                'invalid-expression))

(deftest "records that are not IR still fail as expressions"
  (defstruct point x y)
  (expect-equal (condition-type (fn () (eval (make-point 1 2))))
                'invalid-expression))

(deftest "addressed references read and write the environment"
  (expect-equal
   (prepared-eval
    '(let ((a 1) (b 2))
       (let ((f (fn (c) (set! b (+ b c)) (list a b c))))
         (call f 10)
         (call f 20))))
   '(1 32 20)))

(deftest "an address that no longer fits falls back to lookup"
  (let* ((node (analyze '(let ((x 41)) (+ x 1))))
         (reference (vector-get (ir-call-arguments (ir-let-body node)) 0)))
    (set-ir-reference-depth! reference 7)
    (expect-equal (eval node) 42)))

(deftest "the base library and compiler run prepared" :slow
  (expect (> (prepare-package! (find-package "WISP")) 200))
  (expect-equal (map (fn (x) (* x x)) '(1 2 3)) '(1 4 9))
  (expect-equal (filter '(1 2 3 4) (fn (x) (eq? 0 (mod x 2)))) '(2 4))
  (expect-equal (try (error 'boom 1) (catch (e k) (type-of e))) 'boom)
  (expect-equal (call-with-effect-handler 'ask
                  (fn () (+ (send! 'ask 2) (send! 'ask 3)))
                  (fn (request resume raise) (call resume (* request 10))))
                50)
  (defstruct pair-of left right)
  (expect-equal (pair-of-right (make-pair-of 1 2)) 2)
  (expect-equal `(a ,(+ 1 2) ,@(list 4 5)) '(a 3 4 5))
  ;; The prepared compiler analyzes and runs code, itself included.
  (expect-equal (prepared-eval '(let ((x 2)) (* x 21))) 42)
  (expect (> (prepare-package! (find-package "WISP")) 200)))
