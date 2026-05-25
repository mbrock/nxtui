#lang racket/base

(require "../rdf-forge/ontology.rkt")

(provide nxt
         resource
         deck
         wand
         zone
         task
         wish
         waiter
         deed
         waves
         belongs-to
         is-ready-on
         continues-as
         wants
         holds
         staged-on
         parked-on)

(define-ontology nxt "https://swa.sh/nxt#")

(define-class nxt resource #:abstract)
(define-class nxt deck #:subclass-of resource)
(define-class nxt wand #:subclass-of resource)
(define-class nxt zone #:subclass-of resource)
(define-class nxt task #:subclass-of resource)
(define-class nxt wish #:subclass-of resource)
(define-class nxt waiter #:subclass-of resource)
(define-class nxt deed #:subclass-of resource)

(define-property nxt waves ((deck wand)))
(define-property nxt belongs-to
  ((task zone)
   (waiter task)
   (deed task)
   (deed zone)))
(define-property nxt is-ready-on ((task deck)))
(define-property nxt continues-as ((task task)))
(define-property nxt wants ((waiter wish)))
(define-property nxt holds ((waiter wand)))
(define-property nxt staged-on ((waiter wand)))
(define-property nxt parked-on ((waiter wand)))

(module+ main
  (display (ontology->turtle nxt)))
