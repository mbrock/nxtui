#lang racket/base

(require "../rdf-forge/cli.rkt"
         "runtime.rkt")

(provide runtime-model)

(module+ main
  (run-model-cli runtime-model
                 #:program "nxtrt-model"
                 #:default-run 'rich-runtime-shape-witness))
