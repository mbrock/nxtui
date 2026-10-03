#lang racket/base

;; We compile selected runtime modules, never package documentation or tests.
;; Keep raco's strict dependency validation, but omit build-only declarations
;; from the copied metadata, matching update-racket-sources.rkt's policy.
(require racket/file racket/match)

(define (runtime-info datum)
  (match datum
    [`(define build-deps ,_) '(define build-deps '())]
    [(? pair?) (cons (runtime-info (car datum)) (runtime-info (cdr datum)))]
    [_ datum]))

(for ([dir (in-vector (current-command-line-arguments))])
  (define path (build-path dir "info.rkt"))
  (when (file-exists? path)
    (define datum
      (parameterize ([read-accept-reader #t])
        (call-with-input-file path read)))
    (call-with-output-file path
      (lambda (out) (write (runtime-info datum) out) (newline out))
      #:exists 'truncate)))
