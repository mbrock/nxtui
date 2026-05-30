#lang racket/base

(require (except-in something/base => ==)
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
(def-operator / 900 left join)
(def-operator \|\| 1 prefix disjunct)
(def-operator && 1 prefix conjunct)
(def-operator |\/| 1 prefix disjunct)
(def-operator |/\| 1 prefix conjunct)
(def-operator forall #f prefix-macro forall)
(def-operator exists #f prefix-macro exists)

(provide (all-from-out something/base
                       something/infix
                       "../something.rkt"))
