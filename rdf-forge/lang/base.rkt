#lang racket/base

(require (except-in something/base => == not block)
         something/infix
         "../something.rkt")

(def-operator => 10 right =>)
(def-operator \|\| 20 n-ary ||)
(def-operator && 30 n-ary &&)
(def-operator |\/| 20 n-ary ||)
(def-operator |/\| 30 n-ary &&)
(def-operator == 40 nonassoc ==)
(def-operator = 40 nonassoc ==)
(def-operator in 40 nonassoc in)
(def-operator union 45 n-ary union)
(def-operator intersect 50 n-ary intersect)
(def-operator & 50 n-ary intersect)
(def-operator matching 850 nonassoc matching)
(def-operator ~ 850 nonassoc matching)
(def-operator / 900 left join)
(def-operator \|\| #f prefix-macro disjunct-prefix)
(def-operator && #f prefix-macro conjunct-prefix)
(def-operator |\/| #f prefix-macro disjunct-prefix)
(def-operator |/\| #f prefix-macro conjunct-prefix)
(def-operator forall #f prefix-macro forall)
(def-operator exists #f prefix-macro exists)

(provide (all-from-out something/base
                       something/infix
                       "../something.rkt"))
