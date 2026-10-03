#lang racket/base

(require "../rdf-forge/ontology.rkt"
         "runtime.rkt")

;; Keep the compatibility entrypoint in sync with the single source of the
;; runtime vocabulary, rather than maintaining a second list of its names.
(provide (all-from-out "runtime.rkt"))

(module+ main
  (display (ontology->turtle nxt)))
