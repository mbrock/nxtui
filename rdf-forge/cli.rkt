#lang racket/base

(require racket/cmdline
         "model.rkt")

(provide run-model-cli)

(define (run-model-cli runtime-model
                       #:program [program "forge-model"]
                       #:default-run [default-run 'rich-runtime-shape-witness])
  (define direct-xml-path #f)
  (define run-name default-run)
  (define run-checks? #f)
  (command-line
   #:program program
   #:once-each
   [("--direct-xml") path "Run the model through Forge directly and write Alloy XML"
                    (set! direct-xml-path path)]
   [("--run") name "Run name to execute with --direct-xml"
              (set! run-name (string->symbol name))]
   [("--check") "Run the model checks through Forge directly"
                (set! run-checks? #t)])
  (cond
    [run-checks?
     (void (check-forge-model runtime-model))]
    [direct-xml-path
     (void (run-forge-model runtime-model run-name
                            #:run-sterling 'off
                            #:export-xml direct-xml-path))]
    [else
     (error program "choose --direct-xml or --check")]))
