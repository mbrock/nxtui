#lang racket/base

(require "../rdf-forge/ontology.rkt"
         "runtime.rkt")

(provide nxt
         deck
         zone
         task
         wish
         exec
         exec-state
         prepared-state
         parked-state
         settled-state
         retired-state
         parked-phase
         queued-phase
         submitted-phase
         cancelling-phase
         settled-phase
         ready-to-retire-phase
         draining-phase
         deed
         has-ready
         has-lifecycle
         has-parked-phase
         has-settled-phase
         spawned
         issued
         observes
         has-continuation
         realizes)

(module+ main
  (display (ontology->turtle nxt)))
