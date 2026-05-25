#lang racket/base

(require racket/cmdline
         racket/file
         racket/list
         racket/match
         racket/string
         sxml
         sxml/sxpath
         (only-in "../rdf-forge/model.rkt"
                  forge-model-signatures
                  forge-model-predicates
                  forge-model-checks
                  forge-model-runs
                  forge-signature-term
                  forge-signature-fields
                  forge-field-term
                  forge-field-multiplicity
                  forge-field-range
                  forge-field-variable?
                  forge-field-ref?
                  forge-field-ref-name
                  forge-predicate-name
                  forge-predicate-body
                  forge-check-name
                  forge-run-name
                  forge-run-scope
                  forge-expr?
                  forge-expr-op
                  forge-expr-args
                  forge-quant?
                  forge-quant-kind
                  forge-quant-bindings
                  forge-quant-body)
         "../rdf-forge/ontology.rkt"
         "../nxtrt/model.rkt"
         "../nxtrt/ontology.rkt")

(define (read-xml path)
  (call-with-input-file path
    (lambda (in)
      (ssax:xml->sxml in '()))))

(define (write-xml doc path)
  (make-parent-directory* path)
  (call-with-output-file path
    (lambda (out)
      (display "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" out)
      (srl:sxml->xml doc out)
      (newline out))
    #:exists 'replace))

(define (attr-ref node name [fallback #f])
  (define attrs
    (match node
      [(list _ (list '@ attrs ...) _ ...) attrs]
      [_ '()]))
  (define found (assoc name attrs))
  (if found (cadr found) fallback))

(define (children node)
  (match node
    [(list _ (list '@ _ ...) rest ...) rest]
    [(list _ rest ...) rest]
    [_ '()]))

(define (element? value name)
  (and (pair? value) (eq? (car value) name)))

(define (first-element node name)
  (findf (lambda (child) (element? child name)) (children node)))

(define (child-elements node name)
  (filter (lambda (child) (element? child name)) (children node)))

(define (text-content node)
  (cond
    [(string? node) node]
    [(pair? node) (string-join (map text-content (children node)) "")]
    [else ""]))

(define (only-title? node)
  (element? node 'title))

(define (nonempty-string value)
  (define cleaned (regexp-replace* #px"\\s+" value " "))
  (and (regexp-match? #px"\\S" cleaned) cleaned))

(define (htmlonly->graph node)
  (define html (text-content node))
  (define (attr name)
    (define match
      (regexp-match (pregexp (format "~a=\"([^\"]*)\"" name)) html))
    (and match (cadr match)))
  (and (regexp-match? #px"<forge-doc-graph\\b" html)
       `(forge-graph (@ (frg ,(or (attr "frg") ""))
                        (run ,(or (attr "run") ""))
                        (title ,(or (attr "title") ""))))))

(define (xml-forge-graph->graph node)
  `(forge-graph (@ (frg ,(attr-ref node 'frg ""))
                   (run ,(attr-ref node 'run ""))
                   (title ,(attr-ref node 'title "")))))

(define (convert-inline node)
  (cond
    [(string? node)
     (or (nonempty-string node) "")]
    [(element? node 'computeroutput)
     `(code ,@(filter values (map convert-inline (children node))))]
    [(element? node 'ref)
     `(xref (@ (refid ,(attr-ref node 'refid ""))
               (kind ,(attr-ref node 'kindref "")))
            ,@(filter values (map convert-inline (children node))))]
    [(element? node 'htmlonly)
     (htmlonly->graph node)]
    [(element? node 'forge-graph)
     (xml-forge-graph->graph node)]
    [(pair? node)
     (filter values (map convert-inline (children node)))]
    [else #f]))

(define (flatten-inline values)
  (define (flatten-one value)
    (cond
      [(not value) '()]
      [(and (string? value) (string=? value "")) '()]
      [(and (list? value)
            (not (null? value))
            (symbol? (car value)))
       (list value)]
      [(list? value) (append-map flatten-one value)]
      [else (list value)]))
  (append-map flatten-one values))

(define (convert-para node)
  (define block-children
    (filter (lambda (child)
              (or (element? child 'itemizedlist)
                  (element? child 'sect1)))
            (children node)))
  (cond
    [(pair? block-children)
     `(block ,@(filter values (map convert-block block-children)))]
    [else
     (define converted (flatten-inline (map convert-inline (children node))))
     (define graph-only
       (and (= (length converted) 1)
            (pair? (first converted))
            (eq? (car (first converted)) 'forge-graph)
            (first converted)))
     (cond
       [graph-only graph-only]
       [(null? converted) #f]
       [else `(paragraph ,@converted)])]))

(define (convert-list node)
  `(list ,@(for/list ([item (in-list (child-elements node 'listitem))])
             `(item ,@(filter values (map convert-block (children item)))))))

(define (convert-section node)
  (define title (text-content (first-element node 'title)))
  `(section (@ (id ,(attr-ref node 'id "")))
            (title ,title)
            ,@(filter values
                      (map convert-block
                           (filter-not only-title? (children node))))))

(define (convert-block node)
  (cond
    [(element? node 'para) (convert-para node)]
    [(element? node 'sect1) (convert-section node)]
    [(element? node 'itemizedlist) (convert-list node)]
    [(and (pair? node) (not (eq? (car node) '@)))
     (let ([nested (filter values (map convert-block (children node)))])
       (and (pair? nested) `(block ,@nested)))]
    [else #f]))

(define (page->nice page)
  (define compound ((sxpath "//compounddef") page))
  (unless (= (length compound) 1)
    (error 'page->nice "expected one compounddef, got ~a" (length compound)))
  (define node (first compound))
  (define detail (first-element node 'detaileddescription))
  `(doc-page (@ (id ,(attr-ref node 'id ""))
                (kind ,(attr-ref node 'kind "")))
             (title ,(text-content (first-element node 'title)))
             (source ,(attr-ref (first-element node 'location) 'file ""))
             ,@(filter values (map convert-block (children detail)))
             ,@(runtime-model-doc)))

(define (symbol-title value)
  (string-titlecase
   (string-replace (symbol->string value) "-" " ")))

(define (term-display-name value)
  (symbol->string (term-rdf-name value)))

(define (term-option-ref value key [fallback #f])
  (define found (assoc key (term-options value)))
  (if found (cdr found) fallback))

(define (class-term? value)
  (and (term? value) (eq? (term-kind value) 'class)))

(define (property-term? value)
  (and (term? value) (eq? (term-kind value) 'property)))

(define (field-relation-term signature-term fld)
  (with-handlers ([exn:fail? (lambda (_error) #f)])
    (ontology-property (term-ontology signature-term)
                       (if (forge-field-ref? (forge-field-term fld))
                           (forge-field-ref-name (forge-field-term fld))
                           (term-rdf-name (forge-field-term fld)))
                       #:domain signature-term
                       #:range (forge-field-range fld))))

(define (field-name signature-term fld)
  (define relation-term
    (field-relation-term signature-term fld))
  (if relation-term
      (term-display-name relation-term)
      (format "~a" (forge-field-term fld))))

(define (dexp-symbol name)
  `(dexp-symbol (@ (name ,(format "~a" name))
                   (display ,(case name
                               [(==) "="]
                               [(=>) "⇒"]
                               [(&&) "∧"]
                               [(||) "∨"]
                               [(in) "∈"]
                               [(no) "∅"]
                               [(some) "∃"]
                               [(all) "∀"]
                               [(lone) "≤1"]
                               [(one) "1"]
                               [(ge) "≥"]
                               [else (format "~a" name)])))))

(define (dexp-number value)
  `(dexp-number (@ (value ,(format "~a" value)))))

(define (dexp-string value)
  `(dexp-string (@ (value ,value))))

(define (dexp-list #:callee [callee #f] . children)
  `(dexp-list (@ (callee ,(if callee (format "~a" callee) "")))
              ,@children))

(define (call-doc callee . args)
  (apply dexp-list #:callee callee (cons (dexp-symbol callee) args)))

(define (expr-doc expr)
  (cond
    [(symbol? expr) (dexp-symbol expr)]
    [(number? expr) (dexp-number expr)]
    [(term? expr) (dexp-symbol (term-display-name expr))]
    [(forge-expr? expr)
     (apply dexp-list
            #:callee (forge-expr-op expr)
            (cons (dexp-symbol (forge-expr-op expr))
                  (map expr-doc (forge-expr-args expr))))]
    [(forge-quant? expr)
     (dexp-list
      #:callee (forge-quant-kind expr)
      (dexp-symbol (forge-quant-kind expr))
      (apply dexp-list
             #:callee 'bindings
             (for/list ([binding (in-list (forge-quant-bindings expr))])
               (dexp-list #:callee 'binding
                          (dexp-symbol (car binding))
                          (expr-doc (cdr binding)))))
      (expr-doc (forge-quant-body expr)))]
    [(list? expr)
     (apply dexp-list #:callee 'list (map expr-doc expr))]
    [else (dexp-symbol (format "~a" expr))]))

(define (predicate-doc pred)
  (call-doc 'predicate
            (dexp-symbol (forge-predicate-name pred))
            (expr-doc (forge-predicate-body pred))))

(define (check-doc chk)
  (call-doc 'check (dexp-symbol (forge-check-name chk))))

(define (class-dexp value)
  (apply call-doc
         'class
         (append (list (dexp-symbol (term-display-name value)))
                 (if (term-option-ref value 'abstract)
                     (list (dexp-symbol '#:abstract))
                     '())
                 (if (term-option-ref value 'subclass-of)
                     (list (call-doc '#:subclass-of
                                     (dexp-symbol (term-display-name (term-option-ref value 'subclass-of)))))
                     '()))))

(define (property-dexp value)
  (call-doc 'property
            (dexp-symbol (term-display-name value))
            (dexp-symbol (term-display-name (term-option-ref value 'domain)))
            (dexp-symbol (term-display-name (term-option-ref value 'range)))))

(define (ontology-dexp ont)
  (define terms (ontology-declared-terms ont))
  (call-doc 'ontology
            (dexp-symbol (ontology-prefix ont))
            (dexp-string (ontology-base ont))
            (apply call-doc 'classes (map class-dexp (filter class-term? terms)))
            (apply call-doc 'relations (map property-dexp (filter property-term? terms)))))

(define (field-dexp signature-term fld)
  (apply call-doc
         (string->symbol (field-name signature-term fld))
         (append (list (dexp-symbol (string->symbol (field-name signature-term fld))))
                 (if (forge-field-variable? fld)
                     (list (dexp-symbol 'var))
                     '())
                 (list (dexp-symbol (forge-field-multiplicity fld))
                       (dexp-symbol (term-display-name (forge-field-range fld)))))))

(define (signature-dexp sig)
  (apply call-doc
         'signature
         (cons (dexp-symbol (term-display-name (forge-signature-term sig)))
               (for/list ([fld (in-list (forge-signature-fields sig))])
                 (field-dexp (forge-signature-term sig) fld)))))

(define (model-dexp ont frg-path mdl)
  (call-doc 'runtime-model
            (ontology-dexp ont)
            (apply call-doc 'signatures (map signature-dexp (forge-model-signatures mdl)))
            (apply call-doc 'predicates (map predicate-doc (forge-model-predicates mdl)))
            (apply call-doc 'checks (map check-doc (forge-model-checks mdl)))))

(define (run-doc frg-path run-command)
  (define run-name (symbol->string (forge-run-name run-command)))
  `(run (@ (name ,run-name)
           (scope ,(format "~a" (forge-run-scope run-command))))
        (forge-graph (@ (frg ,frg-path)
                        (run ,run-name)
                        (title ,(case (forge-run-name run-command)
                                  [(rich-runtime-shape-witness)
                                   "Runtime shape"]
                                  [(rich-runtime-trace-witness)
                                   "Runtime trace"]
                                  [else
                                   (symbol-title (forge-run-name run-command))]))))))

(define (model-doc frg-path mdl)
  (define shown-runs '(rich-runtime-shape-witness rich-runtime-trace-witness))
  `(model-section
    ,(model-dexp nxt frg-path mdl)
    (runs
     ,@(for/list ([run-command (in-list (filter (lambda (run-command)
                                                  (member (forge-run-name run-command) shown-runs))
                                                (forge-model-runs mdl)))])
         (run-doc frg-path run-command)))))

(define (runtime-model-doc)
  (list (model-doc "nxtrt/model.rkt" runtime-model)))

(module+ main
  (define input "docs2/out/doxygen/xml/rt_overview.xml")
  (define output "docs2/out/rt-overview.xml")
  (command-line
   #:program "doxygen-to-nice"
   #:once-each
   [("--input") path "Doxygen XML page to read"
                (set! input path)]
   [("--output") path "Nice XML file to write"
                 (set! output path)])
  (write-xml (page->nice (read-xml input)) output)
  (printf "wrote ~a\n" output))
