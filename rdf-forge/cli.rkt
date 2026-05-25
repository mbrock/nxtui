#lang racket/base

(require racket/cmdline
         racket/string
         "model.rkt")

(provide run-model-cli)

(define (run-model-cli runtime-model
                       #:program [program "forge-model"]
                       #:default-run [default-run 'rich-runtime-shape-witness])
  (define direct-xml-path #f)
  (define run-name default-run)
  (define run-all? #f)
  (define run-checks? #f)
  (define run-text? #f)
  (command-line
   #:program program
   #:once-each
   [("--direct-xml") path "Run the model through Forge directly and write Alloy XML"
                    (set! direct-xml-path path)]
   [("--text") "Run the model through Forge directly and print a text instance"
               (set! run-text? #t)]
   [("--run-all") "Run every model run block and print text instances"
                  (set! run-all? #t)]
   [("--run") name "Run name to execute with --direct-xml or --text"
              (set! run-name (string->symbol name))]
   [("--check") "Run the model checks through Forge directly"
                (set! run-checks? #t)])
  (cond
    [run-checks?
     (void (check-forge-model runtime-model))]
    [run-all?
     (define texts
       (parameterize ([current-output-port (open-output-string)])
         (define runs
           (model->forge-runs runtime-model
                              #:run-sterling 'off
                              #:export-run #f
                              #:export-xml #f))
         (for/list ([run-command (in-list (forge-model-runs runtime-model))])
           (forge-run->text
            (hash-ref runs (forge-run-name run-command))))))
     (display (string-join texts "\n"))]
    [run-text?
     (define text
       (parameterize ([current-output-port (open-output-string)])
         (forge-run->text
          (run-forge-model runtime-model run-name
                           #:run-sterling 'off))))
     (display text)]
    [direct-xml-path
     (void (run-forge-model runtime-model run-name
                            #:run-sterling 'off
                            #:export-xml direct-xml-path))]
    [else
     (error program "choose --run-all, --text, --direct-xml, or --check")]))
