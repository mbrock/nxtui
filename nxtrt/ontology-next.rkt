#lang racket/base

(require "../rdf-forge/ontology.rkt"
         "runtime-next.rkt")

(provide nxt-next
         TASK
         DECK
         FLAP
         GAME
         BTHREAD
         CARD
         SYNC
         MOOD
         DAWN
         NOON
         DUSK
         DEAD
         holds
         tries
         picks
         feels
         runs
         offers
         chooses
         at
         asks
         waits
         blocks
         then)

(module+ main
  (display (ontology->turtle nxt-next)))
