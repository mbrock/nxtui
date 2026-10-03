;; -*- mode: wisp; fill-column: 64; -*-
;;; The Wisp compiler, after RFC 0020.
;;
;; Analysis turns source forms into semantic records: ordinary
;; DEFSTRUCT instances that make bindings, evaluation order, and
;; control flow explicit. Nothing here executes those records
;; yet; the source interpreter is still the only evaluator.
;;
;; (analyze form) returns the record for one form, and
;; (ir-show node) describes a record graph as a readable list.
;;
;; This file is part of Wisp.
;;
;; Wisp is free software: you can redistribute it and/or modify
;; it under the terms of the GNU Affero General Public License
;; as published by the Free Software Foundation, either version
;; 3 of the License, or (at your option) any later version.
;;
;; Wisp is distributed in the hope that it will be useful, but
;; WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
;; GNU Affero General Public License for more details.
;;
;; You should have received a copy of the GNU Affero General
;; Public License along with Wisp. If not, see
;; <https://www.gnu.org/licenses/>.



;;; * Semantic records

;; A lexical binder. It is a compiler object, not the runtime
;; location its binder creates each time it executes. OWNER is
;; the analyzed function or scope that introduces it.
(defstruct ir-binding name owner)

;; A use of a known lexical binding. Every use of one binder
;; shares its IR-BINDING object.
(defstruct ir-reference binding)

;; A value lookup that analysis did not resolve lexically: the
;; runtime searches lexical scope, then dynamic bindings and the
;; symbol's value cell, as source evaluation does.
(defstruct ir-lookup symbol)

;; A read of a symbol's function cell.
(defstruct ir-function-reference symbol)

(defstruct ir-constant value)
(defstruct ir-assignment target value)

;; ARGUMENTS is a vector, evaluated left to right after the
;; callee is resolved.
(defstruct ir-call callee arguments)

(defstruct ir-branch test consequent alternative)

;; FORMS is a vector of at least two nodes; the last is in tail
;; position.
(defstruct ir-sequence forms)

(defstruct ir-let bindings initializers body)
(defstruct ir-function name parameters bindings body)
(defstruct ir-closure function)

;; An explicit escape to source evaluation, for forms the
;; analyzer does not support. Malformed forms escape too, so they
;; fail when and how source evaluation fails.
(defstruct ir-source form)



;;; * Analysis

;; The core special operators. Prepared code treats them as
;; fixed control structure; RFC 0020 does not support redefining
;; them. FN, SET!, DEFUN and the rest are macros over these.
(defvar *ir-special-operators*
  '(quote function %fn %macro-fn if do let))

(defun ir-special-operator? (symbol)
  (find *ir-special-operators* (fn (x) (eq? x symbol))))

;; The length of a proper list, or NIL for an improper or cyclic
;; one, which analysis leaves to source evaluation.
(defun %ir-list-length (xs)
  (%ir-list-length-loop xs xs 0))

(defun %ir-list-length-loop (slow fast n)
  (cond ((nil? fast) n)
        ((not (pair? fast)) nil)
        ((nil? (tail fast)) (+ n 1))
        ((not (pair? (tail fast))) nil)
        (t (let ((slow (tail slow))
                 (fast (tail (tail fast))))
             (if (eq? slow fast) nil
               (%ir-list-length-loop slow fast (+ n 2)))))))

;; A symbol that can name a function or variable: not NIL, T,
;; or a keyword.
(defun %ir-name? (x)
  (and (symbol? x)
       (not (nil? x))
       (not (eq? x t))
       (not (eq? (symbol-package x) (find-package "KEYWORD")))))

;; The atoms that source evaluation returns unchanged.
(defun %ir-self-evaluating? (x)
  (or (nil? x) (eq? x t) (integer? x) (string? x) (vector? x)
      (and (symbol? x) (not (%ir-name? x)))))

;; SCOPE lists the IR-BINDINGs in scope, innermost first.
(defun %ir-resolve (symbol scope)
  (let ((found (find scope
                     (fn (binding)
                       (eq? (ir-binding-name binding) symbol)))))
    (if found
        (make-ir-reference (head found))
      (make-ir-lookup symbol))))

(defun analyze (form &optional scope)
  (cond ((%ir-self-evaluating? form) (make-ir-constant form))
        ((symbol? form) (%ir-resolve form scope))
        ((pair? form) (%ir-analyze-form form scope))
        (t (make-ir-source form))))

(defun %ir-analyze-list (forms scope)
  (map (fn (form) (analyze form scope)) forms))

;; A list form is a special form, a macro use, or a call. As in
;; Common Lisp, a head that names neither a special operator nor
;; a macro at analysis time is a call, defined yet or not.
(defun %ir-analyze-form (form scope)
  (let ((operator (head form))
        (count (%ir-list-length (tail form))))
    (cond ((not (and (%ir-name? operator) count))
           (make-ir-source form))
          ((ir-special-operator? operator)
           (%ir-analyze-special operator form count scope))
          ((eq? 'macro (type-of (symbol-function operator)))
           (analyze (macroexpand-1 form) scope))
          (t (make-ir-call
              (make-ir-function-reference operator)
              (vector-from-list
               (%ir-analyze-list (tail form) scope)))))))

(defun %ir-analyze-special (operator form count scope)
  (let ((arguments (tail form)))
    (cond ((and (eq? operator 'quote) (eq? count 1))
           (make-ir-constant (head arguments)))
          ((and (eq? operator 'function) (eq? count 1)
                (%ir-name? (head arguments)))
           (make-ir-function-reference (head arguments)))
          ((and (eq? operator 'if) (eq? count 3))
           (make-ir-branch (analyze (head arguments) scope)
                           (analyze (second arguments) scope)
                           (analyze (third arguments) scope)))
          ((eq? operator 'do)
           (%ir-analyze-body arguments scope))
          (t (make-ir-source form)))))

(defun %ir-analyze-body (forms scope)
  (cond ((nil? forms) (make-ir-constant nil))
        ((nil? (tail forms)) (analyze (head forms) scope))
        (t (make-ir-sequence
            (vector-from-list (%ir-analyze-list forms scope))))))



;;; * Inspection

;; (ir-show node) describes a graph as nested lists headed by
;; keywords. Bindings are numbered in order of first appearance,
;; so every reference to one binding shows the same number:
;;
;;   (:IF (:REFERENCE X 1) (:CALL FOO (:REFERENCE X 1))
;;        (:CONSTANT 17))
(defun ir-show (node)
  (%ir-show node (cons nil nil)))

;; SEEN holds an alist from bindings to their numbers.
(defun %ir-binding-number (binding seen)
  (let ((found (find (head seen)
                     (fn (entry) (eq? (head entry) binding)))))
    (if found
        (tail (head found))
      (let ((n (+ 1 (length (head seen)))))
        (set-head! seen (cons (cons binding n) (head seen)))
        n))))

(defun %ir-show-binding (binding seen)
  (list (ir-binding-name binding)
        (%ir-binding-number binding seen)))

(defun %ir-show-all (nodes seen)
  (map (fn (node) (%ir-show node seen)) (list-from-vector nodes)))

(defun %ir-show-callee (callee seen)
  (if (ir-function-reference? callee)
      (ir-function-reference-symbol callee)
    (%ir-show callee seen)))

(defun %ir-show (node seen)
  (cond
    ((ir-constant? node)
     (list :constant (ir-constant-value node)))
    ((ir-lookup? node)
     (list :lookup (ir-lookup-symbol node)))
    ((ir-reference? node)
     (cons :reference
           (%ir-show-binding (ir-reference-binding node) seen)))
    ((ir-function-reference? node)
     (list :function (ir-function-reference-symbol node)))
    ((ir-call? node)
     (cons :call
           (cons (%ir-show-callee (ir-call-callee node) seen)
                 (%ir-show-all (ir-call-arguments node) seen))))
    ((ir-branch? node)
     (list :if
           (%ir-show (ir-branch-test node) seen)
           (%ir-show (ir-branch-consequent node) seen)
           (%ir-show (ir-branch-alternative node) seen)))
    ((ir-sequence? node)
     (cons :do (%ir-show-all (ir-sequence-forms node) seen)))
    ((ir-source? node)
     (list :source (ir-source-form node)))
    (t (list :unknown node))))
