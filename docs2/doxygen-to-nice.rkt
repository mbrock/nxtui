#lang racket/base

(require racket/cmdline
         racket/file
         racket/list
         racket/match
         racket/string
         sxml
         sxml/sxpath)

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
             ,@(filter values (map convert-block (children detail)))))

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
