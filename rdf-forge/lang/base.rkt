#lang racket/base

(require (except-in something/base => ==)
         something/infix
         "../something.rkt")

(provide (all-from-out something/base
                       something/infix
                       "../something.rkt"))
