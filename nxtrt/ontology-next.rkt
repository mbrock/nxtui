#lang racket/base

(require "../rdf-forge/ontology.rkt"
         "runtime-next.rkt")

(provide nxt-next
         TASK
         GAME
         CARD
         SYNC
         runs
         considers
         chooses
         awaits
         posts
         waits
         halts
         links)

(module+ main
  (display (ontology->turtle nxt-next)))
