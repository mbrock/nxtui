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
         waves-wand
         belongs-to-zone
         belongs-to-deck
         notifies
         continues-as
         wants-wish
         holds-wand
         waits-on-task
         is-staged-on-wand
         is-parked-on-wand
         records-task
         happens-in-zone)

(define-ontology nxt "https://swa.sh/nxt#")

(define-class nxt resource #:abstract)
(define-class nxt deck #:subclass-of resource)
(define-class nxt wand #:subclass-of resource)
(define-class nxt zone #:subclass-of resource)
(define-class nxt task #:subclass-of resource)
(define-class nxt wish #:subclass-of resource)
(define-class nxt waiter #:subclass-of resource)
(define-class nxt deed #:subclass-of resource)

(define-property nxt waves-wand #:domain deck #:range wand)
(define-property nxt belongs-to-zone #:domain task #:range zone)
(define-property nxt belongs-to-deck #:domain task #:range deck)
(define-property nxt notifies #:domain task #:range waiter)
(define-property nxt continues-as #:domain task #:range task)
(define-property nxt wants-wish #:domain waiter #:range wish)
(define-property nxt holds-wand #:domain waiter #:range wand)
(define-property nxt waits-on-task #:domain waiter #:range task)
(define-property nxt is-staged-on-wand #:domain waiter #:range wand)
(define-property nxt is-parked-on-wand #:domain waiter #:range wand)
(define-property nxt records-task #:domain deed #:range task)
(define-property nxt happens-in-zone #:domain deed #:range zone)

(module+ main
  (display (ontology->turtle nxt)))
