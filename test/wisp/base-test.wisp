;; -*- mode: wisp; fill-column: 64; -*-
;;; The base library (src/wisp/base.wisp).

(deftest "COND clause bodies are an implicit DO"
  (let ((log nil))
    (expect-equal (cond ((eq? 1 2) (set! log (cons 'skipped log)) 'no)
                        (t (set! log (cons 'first log))
                           (set! log (cons 'second log))
                           'yes))
                  'yes)
    (expect-equal log '(second first))))

(deftest "a COND clause with only a test returns its value once"
  (let ((calls 0))
    (expect-equal (cond ((eq? 1 2))
                        ((do (set! calls (+ calls 1)) 42))
                        (t 'unreached))
                  42)
    (expect-equal calls 1)
    (expect-equal (cond (nil) (nil)) nil)))

(deftest "COND without a matching clause is NIL"
  (expect-equal (cond ((eq? 1 2) 'no)) nil)
  (expect-equal (cond) nil))
