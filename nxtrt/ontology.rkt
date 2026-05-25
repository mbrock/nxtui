#lang racket/base

(require "../rdf-forge/ontology.rkt"
         "runtime.rkt")

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

(module+ main
  (display (ontology->turtle nxt)))
