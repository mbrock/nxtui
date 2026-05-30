#lang racket/base

(require (only-in parser-tools/lex position-line position-col position-offset)
         racket/match
         racket/port
         racket/string
         something/reader
         syntax/strip-context)

(provide read-syntax)

(define (form->syntax src form)
  (define (->syntax v pos)
    (datum->syntax #f v (and pos
                             (vector src
                                     (position-line pos)
                                     (position-col pos)
                                     (position-offset pos)
                                     #f))))
  (define (walk-form kids pos)
    (->syntax (map walk kids) pos))
  (define (walk value)
    (match value
      [(list kid)
       (walk kid)]
      [(list kids ...)
       (walk-form kids (and (pair? kids) (token-pos (car kids))))]
      [(token pos 'form kids)
       (walk-form kids pos)]
      [(token pos 'block kids)
       (->syntax (cons #'block (map walk kids)) pos)]
      [(token pos 'sequence kids)
       (->syntax (cons #'#%seq (map walk kids)) pos)]
      [(token pos _ (namespaced-name #f id))
       (->syntax id pos)]
      [(token pos _ (namespaced-name ns id))
       (->syntax (list #'in-module (->syntax ns pos) (->syntax id pos)) pos)]
      [(token pos 'keyword val)
       (->syntax (string->keyword (symbol->string val)) pos)]
      [(token pos (or 'number 'string 'literal) val)
       (->syntax val pos)]))
  (walk form))

(define unicode-replacements
  '(("∀" . " forall ")
    ("∃" . " exists ")
    ("∈" . " in ")
    ("∋" . " matching ")
    ("∧" . " && ")
    ("∨" . " || ")
    ("⇒" . " => ")
    ("→" . " => ")
    ("∩" . " intersect ")
    ("∪" . " union ")))

(define (normalize-unicode-operators text)
  (for/fold ([text text])
            ([replacement (in-list unicode-replacements)])
    (string-replace text (car replacement) (cdr replacement))))

(define (read-syntax src [p (current-input-port)])
  (define forms
    (read-something-forms
     (open-input-string
      (normalize-unicode-operators (port->string p)))))
  (strip-context
   #`(module rdf-forge-module rdf-forge/lang/base
       (#%rewrite-body #,@(map (lambda (form) (form->syntax src form)) forms)))))
