#lang racket/base

;; Compatibility CLI for the double-buffered next nxtrt executable model.

(require "../rdf-forge/cli.rkt"
         "runtime-next.rkt")

(provide runtime-next-model)

(module+ main
  (run-model-cli runtime-next-model
                 #:program "nxtrt-next-model"
                 #:default-run 'R1))
