#lang racket/base

(require "../rdf-forge/ontology.rkt"
         "runtime.rkt")

(provide nxt
         deck
         wand
         zone
         task
         wish
         exec
         deed
         waves
         has-ready
         has-prepared
         has-submitted
         has-parked
         spawned
         issued
         observes
         has-continuation
         realizes)

(module+ main
  (display (ontology->turtle nxt)))
