#lang racket/base

(require (for-syntax racket/base
                     syntax/parse))

(provide
 (struct-out ontology)
 (struct-out term)
 define-ontology
 define-class
 define-property
 ontology-declared-terms
 ontology->turtle
 term-forge-name
 term-iri)

(struct ontology (prefix base terms) #:transparent)
(struct term (ontology local kind options) #:transparent)

(define (make-ontology prefix base)
  (ontology prefix base (box '())))

(define (make-term ont local kind options)
  (define value (term ont local kind options))
  (set-box! (ontology-terms ont) (cons value (unbox (ontology-terms ont))))
  value)

(define (make-class-term ont local #:abstract [abstract #f] #:subclass-of [parent #f])
  (make-term ont local 'class
             (append (if abstract (list (cons 'abstract #t)) '())
                     (if parent (list (cons 'subclass-of parent)) '()))))

(define (make-property-term ont local #:domain [domain #f] #:range [range #f] #:inverse-of [inverse #f])
  (make-term ont local 'property
             (append (if domain (list (cons 'domain domain)) '())
                     (if range (list (cons 'range range)) '())
                     (if inverse (list (cons 'inverse-of inverse)) '()))))

(define (ontology-declared-terms ont)
  (reverse (unbox (ontology-terms ont))))

(define (term-forge-name value)
  (symbol->string (term-local value)))

(define (term-iri value)
  (string-append (ontology-base (term-ontology value))
                 (symbol->string (term-local value))))

(define (turtle-name value)
  (format "~a:~a"
          (ontology-prefix (term-ontology value))
          (term-local value)))

(define (turtle-literal value)
  (cond
    [(term? value) (turtle-name value)]
    [(symbol? value) (format "~a" value)]
    [else (format "~s" value)]))

(define (option-ref options key [fallback #f])
  (define found (assoc key options))
  (if found (cdr found) fallback))

(define (term->turtle value)
  (define options (term-options value))
  (case (term-kind value)
    [(class)
     (string-append
      (format "~a a owl:Class .\n" (turtle-name value))
      (if (option-ref options 'subclass-of)
          (format "~a rdfs:subClassOf ~a .\n"
                  (turtle-name value)
                  (turtle-literal (option-ref options 'subclass-of)))
          ""))]
    [(property)
     (string-append
      (format "~a a owl:ObjectProperty .\n" (turtle-name value))
      (if (option-ref options 'domain)
          (format "~a rdfs:domain ~a .\n"
                  (turtle-name value)
                  (turtle-literal (option-ref options 'domain)))
          "")
      (if (option-ref options 'range)
          (format "~a rdfs:range ~a .\n"
                  (turtle-name value)
                  (turtle-literal (option-ref options 'range)))
          "")
      (if (option-ref options 'inverse-of)
          (format "~a owl:inverseOf ~a .\n"
                  (turtle-name value)
                  (turtle-literal (option-ref options 'inverse-of)))
          ""))]
    [else ""]))

(define (ontology->turtle ont)
  (string-append
   (format "@prefix ~a: <~a> .\n" (ontology-prefix ont) (ontology-base ont))
   "@prefix owl: <http://www.w3.org/2002/07/owl#> .\n"
   "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n\n"
   (apply string-append (map term->turtle (ontology-declared-terms ont)))))

(define-syntax-rule (define-ontology name base)
  (define name (make-ontology 'name base)))

(define-syntax (define-class stx)
  (syntax-parse stx
    [(_ ont:id name:id)
     #'(define name (make-class-term ont 'name))]
    [(_ ont:id name:id #:abstract)
     #'(define name (make-class-term ont 'name #:abstract #t))]
    [(_ ont:id name:id #:subclass-of parent:id)
     #'(define name (make-class-term ont 'name #:subclass-of parent))]
    [(_ ont:id name:id #:abstract #:subclass-of parent:id)
     #'(define name (make-class-term ont 'name #:abstract #t #:subclass-of parent))]
    [(_ ont:id name:id #:subclass-of parent:id #:abstract)
     #'(define name (make-class-term ont 'name #:abstract #t #:subclass-of parent))]))

(define-syntax (define-property stx)
  (syntax-parse stx
    [(_ ont:id name:id)
     #'(define name (make-property-term ont 'name))]
    [(_ ont:id name:id #:domain domain:id #:range range:id)
     #'(define name (make-property-term ont 'name #:domain domain #:range range))]
    [(_ ont:id name:id #:domain domain:id #:range range:id #:inverse-of inverse:id)
     #'(define name (make-property-term ont 'name #:domain domain #:range range #:inverse-of inverse))]))
