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
;; Analysis builds records mutably while it walks; a finished
;; graph is treated as stable. Derived facts such as captures
;; are computed from the graph rather than stored in it.
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
;; shares its IR-BINDING object. DEPTH and INDEX, when known, are
;; the binding's lexical address: how many runtime scopes to skip
;; and which name/value pair of that scope's vector holds it.
;; RFC 0020 does not support changing an environment's shape, so
;; analysis can compute the address once; a reference to a
;; binding without a runtime scope behind it has none, and looks
;; its name up instead.
(defstruct ir-reference binding (depth nil) (index nil))

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

;; BINDINGS and INITIALIZERS are vectors in clause order. The
;; initializers run left to right in the enclosing scope, and the
;; bindings become visible together in BODY. The LET node owns
;; its bindings.
(defstruct ir-let bindings initializers body)

;; The code of a function literal. PARAMETERS is an
;; IR-PARAMETERS; BINDINGS is a vector of every parameter binding
;; in source order. The function owns its parameter bindings.
;; SOURCE is the body form it was analyzed from: a prepared
;; closure's CODE shows that snapshot, and editing it does not
;; change the analyzed BODY.
(defstruct ir-function name parameters bindings body source)

;; REQUIRED and OPTIONAL are vectors of bindings, and REST is a
;; binding or NIL. Missing optional arguments are NIL; Wisp has
;; no default forms. SOURCE keeps the original parameter list,
;; which runtime argument binding still interprets.
(defstruct ir-parameters required optional rest source)

;; Evaluating a function literal makes a closure: FUNCTION's code
;; with the current lexical environment. Each evaluation makes a
;; new closure over the same code.
(defstruct ir-closure function)

;; An explicit escape to source evaluation, for forms the
;; analyzer does not support. Malformed forms escape too, so they
;; fail when and how source evaluation fails. SCOPE lists the
;; bindings visible at the escape, all of which the source form
;; may name.
(defstruct ir-source form scope)



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

;; SCOPE lists the IR-BINDINGs in scope, innermost first, in the
;; order of their runtime scope vectors. A :FRAME marker ends the
;; bindings of each runtime scope; bindings after the last marker,
;; such as those a caller passes to ANALYZE, have no runtime scope
;; and so no address.
(defun %ir-resolve (symbol scope)
  (%ir-resolve-loop symbol scope 0 0))

(defun %ir-resolve-loop (symbol scope depth index)
  (cond ((nil? scope) (make-ir-lookup symbol))
        ((eq? (head scope) :frame)
         (%ir-resolve-loop symbol (tail scope) (+ depth 1) 0))
        ((eq? (ir-binding-name (head scope)) symbol)
         (make-ir-reference (head scope)
                            (%ir-address-depth (tail scope) depth)
                            (%ir-address-depth (tail scope) index)))
        (t (%ir-resolve-loop symbol (tail scope) depth (+ index 1)))))

;; An address part, if a :FRAME marker closes the binding's scope.
(defun %ir-address-depth (rest part)
  (when (%ir-memq :frame rest) part))

;; The scope inside a function, whose parameters form one runtime
;; scope in source order, and inside a LET with clauses, whose
;; runtime scope lists its clauses last first.
(defun %ir-function-scope (bindings scope)
  (append bindings (cons :frame scope)))

(defun %ir-let-scope (bindings scope)
  (reverse-append bindings (cons :frame scope)))

;; The bindings of a scope, without its markers.
(defun %ir-scope-bindings (scope)
  (filter scope (fn (x) (not (eq? x :frame)))))

(defun analyze (form &optional scope)
  (cond ((%ir-self-evaluating? form) (make-ir-constant form))
        ((symbol? form) (%ir-resolve form scope))
        ((pair? form) (%ir-analyze-form form scope))
        (t (make-ir-source form (%ir-scope-bindings scope)))))

(defun %ir-analyze-list (forms scope)
  (map (fn (form) (analyze form scope)) forms))

;; A list form is a special form, a macro use, or a call. As in
;; Common Lisp, a head that names neither a special operator nor
;; a macro at analysis time is a call, defined yet or not.
;; %SET! is an ordinary primitive, but it is open-coded: with a
;; quoted name it is an assignment.
(defun %ir-analyze-form (form scope)
  (let ((operator (head form))
        (count (%ir-list-length (tail form))))
    (cond ((not (and (%ir-name? operator) count))
           (make-ir-source form (%ir-scope-bindings scope)))
          ((ir-special-operator? operator)
           (%ir-analyze-special operator form count scope))
          ((and (eq? operator '%set!) (eq? count 2)
                (%ir-quoted-name? (second form)))
           (make-ir-assignment
            (%ir-resolve (second (second form)) scope)
            (analyze (third form) scope)))
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
          ((and (eq? operator 'let) (> count 0))
           (%ir-analyze-let form (head arguments) (tail arguments) scope))
          ((and (eq? operator '%fn) (eq? count 3))
           (%ir-analyze-function form (head arguments) (second arguments)
                                 (third arguments) scope))
          (t (make-ir-source form (%ir-scope-bindings scope))))))

(defun %ir-quoted-name? (x)
  (and (pair? x)
       (eq? (head x) 'quote)
       (pair? (tail x))
       (nil? (tail (tail x)))
       (%ir-name? (second x))))

(defun %ir-analyze-body (forms scope)
  (cond ((nil? forms) (make-ir-constant nil))
        ((nil? (tail forms)) (analyze (head forms) scope))
        (t (make-ir-sequence
            (vector-from-list (%ir-analyze-list forms scope))))))



;;; * Binders

;; Each LET clause is a list of a name and an initializer.
(defun %ir-let-clauses? (clauses)
  (and (%ir-list-length clauses)
       (not (some? (fn (clause)
                     (not (and (eq? (%ir-list-length clause) 2)
                               (%ir-name? (head clause)))))
                   clauses))))

;; Source LET builds its scope from a reversed accumulator, so a
;; duplicated name resolves to its last clause. Putting the
;; bindings into SCOPE in reverse keeps that lookup order.
(defun %ir-analyze-let (form clauses body scope)
  (cond
    ((not (%ir-let-clauses? clauses))
     (make-ir-source form (%ir-scope-bindings scope)))
    ((nil? clauses) (%ir-analyze-body body scope))
    (t (let* ((node (make-ir-let nil nil nil))
              (bindings (map (fn (clause)
                               (make-ir-binding (head clause) node))
                             clauses)))
         (set-ir-let-bindings! node (vector-from-list bindings))
         (set-ir-let-initializers!
          node
          (vector-from-list
           (map (fn (clause) (analyze (second clause) scope))
                clauses)))
         (set-ir-let-body!
          node
          (%ir-analyze-body body (%ir-let-scope bindings scope)))
         node))))

;; Parse a parameter list as source argument binding does: a
;; (REQUIRED OPTIONAL REST) list of names, or NIL when the list
;; is malformed. &OPTIONAL makes every later name optional, and
;; &REST or &BODY must precede exactly one final name.
(defun %ir-parse-parameters (parameters)
  (when (%ir-list-length parameters)
    (%ir-parse-parameters-loop parameters nil nil nil)))

(defun %ir-parse-parameters-loop (parameters optional? required optional)
  (if (nil? parameters)
      (list (reverse required) (reverse optional) nil)
    (%ir-parse-parameter (head parameters) parameters optional?
                         required optional)))

(defun %ir-parse-parameter (p parameters optional? required optional)
  (cond
    ((eq? p '&optional)
     (%ir-parse-parameters-loop (tail parameters) t required optional))
    ((or (eq? p '&rest) (eq? p '&body))
     (when (and (pair? (tail parameters))
                (nil? (tail (tail parameters)))
                (%ir-name? (second parameters)))
       (list (reverse required) (reverse optional)
             (second parameters))))
    ((not (%ir-name? p)) nil)
    (optional?
     (%ir-parse-parameters-loop (tail parameters) t required
                                (cons p optional)))
    (t
     (%ir-parse-parameters-loop (tail parameters) nil
                                (cons p required) optional))))

;; Source argument binding scans parameters in order, so a
;; duplicated parameter resolves to its first occurrence: the
;; bindings enter SCOPE in source order.
(defun %ir-analyze-function (form name parameters body scope)
  (let ((parsed (%ir-parse-parameters parameters)))
    (if (or (nil? parsed)
            (not (or (nil? name) (%ir-name? name))))
        (make-ir-source form (%ir-scope-bindings scope))
      (let* ((function (make-ir-function name nil nil nil body))
             (bind (fn (parameter) (make-ir-binding parameter function)))
             (required (map bind (head parsed)))
             (optional (map bind (second parsed)))
             (rest (when (third parsed) (call bind (third parsed))))
             (bindings (append required optional
                               (if rest (list rest) nil))))
        (set-ir-function-parameters!
         function
         (make-ir-parameters (vector-from-list required)
                             (vector-from-list optional)
                             rest
                             parameters))
        (set-ir-function-bindings! function (vector-from-list bindings))
        (set-ir-function-body! function
                               (analyze body
                                        (%ir-function-scope bindings scope)))
        (make-ir-closure function)))))



;;; * Running prepared code

;; Evaluating an IR node runs it on the same control machine as
;; source evaluation: its frames are ordinary continuation frames,
;; so prepared and source code call each other, capture and
;; resume continuations, and survive collection and tapes.
;; (prepared-eval form) analyzes FORM and evaluates the result
;; under public EVAL's scope rule. A function literal evaluates to
;; a closure whose calls run prepared code.
(defun prepared-eval (form)
  (eval (analyze form)))

;; Give an existing closure prepared code analyzed from its
;; parameters and source body, and return T, or NIL when the
;; parameters cannot be analyzed. The closure keeps its captured
;; environment: names its body does not bind are runtime lookups,
;; which find that environment as source evaluation would.
(defun prepare-function! (function)
  (let ((node (analyze (list '%fn
                             (function-name function)
                             (function-parameters function)
                             (code function)))))
    (when (ir-closure? node)
      (set-code! function (ir-closure-function node))
      t)))

;; Prepare every function, not macro or primitive, named by a
;; symbol in PACKAGE, and return how many were prepared.
(defun prepare-package! (package)
  (%prepare-each! (package-symbols package) 0))

(defun %prepare-each! (symbols count)
  (if (nil? symbols) count
    (let ((function (symbol-function (head symbols))))
      (%prepare-each!
       (tail symbols)
       (if (and (eq? (type-of function) 'function)
                (not (jet? function))
                (prepare-function! function))
           (+ count 1)
         count)))))



;;; * Captures

;; The bindings that NODE uses without introducing them, each
;; once, in order of first use. A source escape may name any
;; binding visible where it occurs, so it uses all of them.
(defun ir-free-bindings (node)
  (reverse (%ir-free node nil nil)))

;; The bindings a closure or function literal captures from its
;; enclosing scopes, including those that nested functions use.
(defun ir-captures (node)
  (ir-free-bindings
   (if (ir-closure? node) (ir-closure-function node) node)))

(defun %ir-memq (x xs)
  (find xs (fn (y) (eq? x y))))

;; ACC holds the free bindings found so far, newest first.
(defun %ir-note-free (binding introduced acc)
  (if (or (%ir-memq binding introduced) (%ir-memq binding acc))
      acc
    (cons binding acc)))

(defun %ir-free-each (nodes introduced acc)
  (if (nil? nodes) acc
    (%ir-free-each (tail nodes) introduced
                   (%ir-free (head nodes) introduced acc))))

(defun %ir-free (node introduced acc)
  (cond
    ((ir-reference? node)
     (%ir-note-free (ir-reference-binding node) introduced acc))
    ((ir-assignment? node)
     (%ir-free-each (list (ir-assignment-target node)
                          (ir-assignment-value node))
                    introduced acc))
    ((ir-call? node)
     (%ir-free-each (list-from-vector (ir-call-arguments node))
                    introduced
                    (%ir-free (ir-call-callee node) introduced acc)))
    ((ir-branch? node)
     (%ir-free-each (list (ir-branch-test node)
                          (ir-branch-consequent node)
                          (ir-branch-alternative node))
                    introduced acc))
    ((ir-sequence? node)
     (%ir-free-each (list-from-vector (ir-sequence-forms node))
                    introduced acc))
    ((ir-let? node)
     (%ir-free (ir-let-body node)
               (append (list-from-vector (ir-let-bindings node))
                       introduced)
               (%ir-free-each
                (list-from-vector (ir-let-initializers node))
                introduced acc)))
    ((ir-closure? node)
     (%ir-free (ir-closure-function node) introduced acc))
    ((ir-function? node)
     (%ir-free (ir-function-body node)
               (append (list-from-vector (ir-function-bindings node))
                       introduced)
               acc))
    ((ir-source? node)
     (%ir-free-each (map #'make-ir-reference (ir-source-scope node))
                    introduced acc))
    (t acc)))



;;; * Checking

;; (ir-check node &optional scope) checks that a graph is well
;; formed and returns a list of problems, or NIL. Each problem is
;; a list of a keyword, the offending node, and details. SCOPE
;; lists the bindings already visible, as for ANALYZE.
;;
;; The checker follows only executable child edges: it never
;; follows a binding's owner or looks inside a constant, so
;; cyclic literal data is fine, but a node that contains itself
;; is a :CYCLE. It checks node kinds, that every reference and
;; source escape names a binding in scope, that binders own the
;; bindings they introduce and introduce each only once, and that
;; a function's bindings match its parameter list.
(defun ir-check (node &optional scope)
  (let ((state (vector nil nil)))
    (%ir-check node scope nil state)
    (reverse (vector-get state 0))))

(defun ir-valid? (node &optional scope)
  (nil? (ir-check node scope)))

;; STATE is a vector of the problems found so far, newest first,
;; and the bindings introduced so far.
(defun %ir-problem (state &rest problem)
  (vector-set! state 0 (cons problem (vector-get state 0)))
  nil)

(defun %ir-node? (x)
  (some? (fn (predicate) (call predicate x))
         (list #'ir-constant? #'ir-lookup? #'ir-reference?
               #'ir-function-reference? #'ir-assignment? #'ir-call?
               #'ir-branch? #'ir-sequence? #'ir-let? #'ir-closure?
               #'ir-function? #'ir-source?)))

;; Check a node in SCOPE, given its ANCESTORS on the path from
;; the root.
(defun %ir-check (node scope ancestors state)
  (cond
    ((not (%ir-node? node))
     (%ir-problem state :not-a-node node))
    ((%ir-memq node ancestors)
     (%ir-problem state :cycle node))
    (t (%ir-check-node node scope (cons node ancestors) state))))

(defun %ir-check-each (nodes scope ancestors state)
  (for-each nodes
            (fn (node) (%ir-check node scope ancestors state))))

;; The elements of a vector of nodes, or NIL after noting a
;; problem when it is not a vector.
(defun %ir-check-vector (owner nodes state)
  (if (vector? nodes)
      (list-from-vector nodes)
    (%ir-problem state :not-a-vector owner nodes)))

;; A reference's address must be the one its scope implies, so
;; prepared execution reads the slot that name lookup would find.
(defun %ir-check-address (node scope state)
  (let ((binding (ir-reference-binding node)))
    (when (ir-binding? binding)
      (let ((expected (%ir-resolve (ir-binding-name binding) scope)))
        (unless (and (ir-reference? expected)
                     (eq? (ir-reference-binding expected) binding)
                     (equal? (ir-reference-depth expected)
                             (ir-reference-depth node))
                     (equal? (ir-reference-index expected)
                             (ir-reference-index node)))
          (%ir-problem state :wrong-address node
                       (list (ir-reference-depth node)
                             (ir-reference-index node))))))))

(defun %ir-check-binding-use (owner binding scope state)
  (cond ((not (ir-binding? binding))
         (%ir-problem state :not-a-binding owner binding))
        ((not (%ir-memq binding scope))
         (%ir-problem state :out-of-scope owner binding))
        (t nil)))

;; Note that OWNER introduces BINDINGS, and return them.
(defun %ir-check-binders (owner bindings state)
  (for-each bindings
    (fn (binding)
      (cond ((not (ir-binding? binding))
             (%ir-problem state :not-a-binding owner binding))
            ((not (eq? owner (ir-binding-owner binding)))
             (%ir-problem state :wrong-owner owner binding))
            ((not (%ir-name? (ir-binding-name binding)))
             (%ir-problem state :bad-name owner
                          (ir-binding-name binding)))
            ((%ir-memq binding (vector-get state 1))
             (%ir-problem state :introduced-twice owner binding))
            (t (vector-set! state 1
                            (cons binding (vector-get state 1)))))))
  bindings)

(defun %ir-check-node (node scope ancestors state)
  (cond
    ((ir-constant? node) nil)
    ((ir-lookup? node)
     (unless (%ir-name? (ir-lookup-symbol node))
       (%ir-problem state :bad-name node (ir-lookup-symbol node))))
    ((ir-function-reference? node)
     (unless (%ir-name? (ir-function-reference-symbol node))
       (%ir-problem state :bad-name node
                    (ir-function-reference-symbol node))))
    ((ir-reference? node)
     (%ir-check-binding-use node (ir-reference-binding node)
                            scope state)
     (when (%ir-memq (ir-reference-binding node) scope)
       (%ir-check-address node scope state)))
    ((ir-assignment? node)
     (let ((target (ir-assignment-target node)))
       (if (or (ir-reference? target) (ir-lookup? target))
           (%ir-check target scope ancestors state)
         (%ir-problem state :bad-target node target))
       (%ir-check (ir-assignment-value node) scope ancestors state)))
    ((ir-call? node)
     (%ir-check (ir-call-callee node) scope ancestors state)
     (%ir-check-each (%ir-check-vector node (ir-call-arguments node)
                                       state)
                     scope ancestors state))
    ((ir-branch? node)
     (%ir-check-each (list (ir-branch-test node)
                           (ir-branch-consequent node)
                           (ir-branch-alternative node))
                     scope ancestors state))
    ((ir-sequence? node)
     (let ((forms (%ir-check-vector node (ir-sequence-forms node)
                                    state)))
       (when (and (vector? (ir-sequence-forms node))
                  (< (length forms) 2))
         (%ir-problem state :short-sequence node))
       (%ir-check-each forms scope ancestors state)))
    ((ir-let? node) (%ir-check-let node scope ancestors state))
    ((ir-closure? node)
     (let ((function (ir-closure-function node)))
       (if (ir-function? function)
           (%ir-check function scope ancestors state)
         (%ir-problem state :not-a-function node function))))
    ((ir-function? node)
     (%ir-check-function node scope ancestors state))
    ((ir-source? node)
     (let ((visible (ir-source-scope node)))
       (if (%ir-list-length visible)
           (for-each visible
             (fn (binding)
               (%ir-check-binding-use node binding scope state)))
         (%ir-problem state :bad-scope node visible))))))

(defun %ir-check-let (node scope ancestors state)
  (let ((bindings (%ir-check-vector node (ir-let-bindings node) state))
        (initializers (%ir-check-vector node (ir-let-initializers node)
                                        state)))
    (if (or (nil? bindings)
            (not (eq? (length bindings) (length initializers))))
        (%ir-problem state :let-shape node)
      (do
        (%ir-check-binders node bindings state)
        (%ir-check-each initializers scope ancestors state)
        (%ir-check (ir-let-body node)
                   (%ir-let-scope bindings scope)
                   ancestors state)))))

(defun %ir-check-function (node scope ancestors state)
  (let ((name (ir-function-name node))
        (parameters (ir-function-parameters node))
        (bindings (%ir-check-vector node (ir-function-bindings node)
                                    state)))
    (unless (or (nil? name) (%ir-name? name))
      (%ir-problem state :bad-name node name))
    (if (not (and (ir-parameters? parameters)
                  (%ir-parameters-match? parameters bindings)))
        (%ir-problem state :parameter-mismatch node parameters)
      (do
        (%ir-check-binders node bindings state)
        (%ir-check (ir-function-body node)
                   (%ir-function-scope bindings scope)
                   ancestors state)))))

;; The parameter record and the binding vector describe the same
;; bindings, in the order the source parameter list names them.
(defun %ir-parameters-match? (parameters bindings)
  (let ((required (ir-parameters-required parameters))
        (optional (ir-parameters-optional parameters))
        (rest (ir-parameters-rest parameters))
        (parsed (%ir-parse-parameters (ir-parameters-source parameters))))
    (and parsed
         (vector? required)
         (vector? optional)
         (let ((declared (append (list-from-vector required)
                                 (list-from-vector optional)
                                 (if rest (list rest) nil))))
           (and (%ir-same-list? declared bindings)
                (equal? (map #'%ir-binding-name-or-nil declared)
                        (append (head parsed) (second parsed)
                                (if (third parsed)
                                    (list (third parsed))
                                  nil)))
                (eq? (vector-length required) (length (head parsed))))))))

(defun %ir-binding-name-or-nil (x)
  (if (ir-binding? x) (ir-binding-name x) nil))

(defun %ir-same-list? (xs ys)
  (cond ((nil? xs) (nil? ys))
        ((nil? ys) nil)
        ((eq? (head xs) (head ys)) (%ir-same-list? (tail xs) (tail ys)))
        (t nil)))



;;; * Inspection

;; (ir-show node) describes a graph as nested lists headed by
;; keywords. Bindings are numbered in order of first appearance,
;; so every reference to one binding shows the same number:
;;
;;   (:FN NIL ((X 1))
;;     (:IF (:REFERENCE X 1) (:CALL FOO (:REFERENCE X 1))
;;          (:CONSTANT 17)))
;;
;; A function shows its parameters, then :CAPTURES and the
;; bindings it captures, if any, then its body.
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

(defun %ir-show-let (node seen)
  (list :let
        (map (fn (pair)
               (list (%ir-show-binding (head pair) seen)
                     (%ir-show (tail pair) seen)))
             (%ir-zip (list-from-vector (ir-let-bindings node))
                      (list-from-vector (ir-let-initializers node))))
        (%ir-show (ir-let-body node) seen)))

(defun %ir-zip (xs ys)
  (if (nil? xs) nil
    (cons (cons (head xs) (head ys))
          (%ir-zip (tail xs) (tail ys)))))

(defun %ir-show-parameters (parameters seen)
  (let ((show (fn (binding) (%ir-show-binding binding seen)))
        (optional (list-from-vector (ir-parameters-optional parameters)))
        (rest (ir-parameters-rest parameters)))
    (append (map show (list-from-vector
                       (ir-parameters-required parameters)))
            (if optional (cons '&optional (map show optional)) nil)
            (if rest (list '&rest (call show rest)) nil))))

(defun %ir-show-function (kind function seen)
  (let* ((parameters (%ir-show-parameters
                      (ir-function-parameters function) seen))
         (captures (map (fn (binding) (%ir-show-binding binding seen))
                        (ir-captures function))))
    (append (list kind (ir-function-name function) parameters)
            (if captures (list :captures captures) nil)
            (list (%ir-show (ir-function-body function) seen)))))

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
    ((ir-assignment? node)
     (list :set
           (%ir-show (ir-assignment-target node) seen)
           (%ir-show (ir-assignment-value node) seen)))
    ((ir-let? node) (%ir-show-let node seen))
    ((ir-closure? node)
     (%ir-show-function :fn (ir-closure-function node) seen))
    ((ir-function? node) (%ir-show-function :code node seen))
    ((ir-source? node)
     (list :source (ir-source-form node)))
    (t (list :unknown node))))
