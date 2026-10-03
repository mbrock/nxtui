;; -*- mode: wisp; fill-column: 64; -*-
;;; Test harness for Wisp test files.
;;
;; nxt-tests loads this file and then one test file, each in a
;; fresh machine with the base library and the compiler, so a
;; file only declares its tests:
;;
;;   (deftest "addition works"
;;     (expect (eq? 4 (+ 2 2)))
;;     (expect-equal (list 1 2) '(1 2)))
;;
;;   (deftest "a long integration case" :slow
;;     ...)
;;
;; To run test I, the runner loads both files into another fresh
;; machine and calls (%run-test I). Expectations record failures
;; and continue, and an unhandled error fails the test.

;; Declared tests, newest first: (name slow thunk) lists.
(defvar *tests* nil)

;; Failures recorded by the running test, newest first.
(defvar *failures* nil)

(defun %declare-test (name slow thunk)
  (when (find *tests* (fn (test) (equal? (head test) name)))
    (error 'duplicate-test name))
  (set! *tests* (cons (list name slow thunk) *tests*)))

(defmacro deftest (name &rest body)
  (let ((slow (eq? (head body) :slow)))
    `(%declare-test ,name ,slow
                    (fn () ,@(if slow (tail body) body) nil))))

(defun %fail (&rest description)
  (set! *failures* (cons description *failures*))
  nil)

(defmacro expect (form)
  `(if ,form t (%fail 'expected ',form)))

(defmacro expect-equal (actual expected)
  `(%expect-equal ,actual ,expected ',actual))

(defun %expect-equal (actual expected form)
  (if (equal? actual expected) t
    (%fail 'expected expected 'got actual 'from form)))

;; Evaluate a list of top-level forms in order, as a loader
;; would, and return the last value.
(defun run-forms (forms)
  (if (nil? (tail forms))
      (eval (head forms))
    (do (eval (head forms))
        (run-forms (tail forms)))))

;;; The runner's interface

(defun %tests ()
  (reverse *tests*))

(defun %test-names ()
  (map #'head (%tests)))

(defun %test-slow-flags ()
  (map #'second (%tests)))

;; NIL when test I passed; otherwise its failures, oldest first.
(defun %run-test (i)
  (let ((test (%nth (%tests) i)))
    (try (do (call (third test))
             (reverse *failures*))
      (catch (e k)
        (reverse (cons (list 'error e) *failures*))))))

(defun %nth (xs i)
  (if (eq? i 0) (head xs) (%nth (tail xs) (- i 1))))
