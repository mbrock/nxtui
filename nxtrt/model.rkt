#lang racket/base

;; Compatibility CLI for the nxtrt executable model. The actual ontology and
;; Forge-style runtime spec live together in runtime.rkt under #lang rdf-forge.

(require "../rdf-forge/cli.rkt"
         "runtime.rkt")

(provide runtime-model)

(module+ main
  (run-model-cli runtime-model
                 #:program "nxtrt-model"
                 #:default-run 'rich-runtime-shape-witness))
