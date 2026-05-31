#lang racket/base

(require racket/string)

(require (for-syntax racket/base
                     syntax/parse))

(provide
 (struct-out ontology)
 (struct-out term)
 (struct-out ontology-axiom)
 define-ontology
 define-class
 define-variant
 define-disjoint
 define-equivalent-union
 define-equivalent-intersection
 define-subclass-some
 define-subclass-only
 define-property-chain
 define-logical-axiom
 define-annotation-property
 define-property
 ontology-declared-terms
 ontology-property
 ontology-ensure-property
 ontology->turtle
 term-forge-name
 term-rdf-name
 term-iri)

(struct ontology (prefix base terms) #:transparent)
(struct term (ontology local kind options) #:transparent)
(struct ontology-axiom (ontology kind args) #:transparent)

(define (make-ontology prefix base)
  (ontology prefix base (box '())))

(define (make-term ont local kind options)
  (define value (term ont local kind options))
  (set-box! (ontology-terms ont) (cons value (unbox (ontology-terms ont))))
  value)

(define (make-axiom ont kind args)
  (define value (ontology-axiom ont kind args))
  (set-box! (ontology-terms ont) (cons value (unbox (ontology-terms ont))))
  value)

(define (make-class-term ont local
                         #:abstract [abstract #f]
                         #:subclass-of [parent #f]
                         #:variant-children [variant-children #f]
                         #:annotations [annotations '()])
  (make-term ont local 'class
             (append (if abstract (list (cons 'abstract #t)) '())
                     (if parent (list (cons 'subclass-of parent)) '())
                     (if variant-children
                         (list (cons 'variant-children variant-children))
                         '())
                     (if (null? annotations)
                         '()
                         (list (cons 'annotations annotations))))))

(define (make-property-term ont local
                            #:domain [domain #f]
                            #:range [range #f]
                            #:multiplicity [multiplicity #f]
                            #:variable? [variable? #f]
                            #:inverse-of [inverse #f]
                            #:subproperty-of [subproperty #f]
                            #:transitive? [transitive? #f]
                            #:symmetric? [symmetric? #f]
                            #:asymmetric? [asymmetric? #f]
                            #:reflexive? [reflexive? #f]
                            #:irreflexive? [irreflexive? #f]
                            #:functional? [functional? #f]
                            #:inverse-functional? [inverse-functional? #f]
                            #:annotations [annotations '()]
                            #:forge-name [forge-name #f]
                            #:rdf-name [rdf-name #f])
  (make-term ont local 'property
             (append (if domain (list (cons 'domain domain)) '())
                     (if range (list (cons 'range range)) '())
                     (if multiplicity (list (cons 'multiplicity multiplicity)) '())
                     (if variable? (list (cons 'variable? variable?)) '())
                     (if inverse (list (cons 'inverse-of inverse)) '())
                     (if subproperty (list (cons 'subproperty-of subproperty)) '())
                     (if transitive? (list (cons 'transitive? #t)) '())
                     (if symmetric? (list (cons 'symmetric? #t)) '())
                     (if asymmetric? (list (cons 'asymmetric? #t)) '())
                     (if reflexive? (list (cons 'reflexive? #t)) '())
                     (if irreflexive? (list (cons 'irreflexive? #t)) '())
                     (if functional? (list (cons 'functional? #t)) '())
                     (if inverse-functional? (list (cons 'inverse-functional? #t)) '())
                     (if (null? annotations)
                         '()
                         (list (cons 'annotations annotations)))
                     (if forge-name (list (cons 'forge-name forge-name)) '())
                     (if rdf-name (list (cons 'rdf-name rdf-name)) '()))))

(define (overload-forge-name local domain range)
  (string->symbol
   (format "~a-for-~a-~a"
           local
           (term-local domain)
           (term-local range))))

(define (property-overload-domain overload) (list-ref overload 0))
(define (property-overload-range overload) (list-ref overload 1))
(define (property-overload-multiplicity overload)
  (if (>= (length overload) 3) (list-ref overload 2) #f))
(define (property-overload-variable? overload)
  (if (>= (length overload) 4) (list-ref overload 3) #f))

(define (normalize-property-options property-options)
  (sort property-options keyword<? #:key car))

(define (make-property-family ont local overloads #:options [property-options '()])
  (define normalized-options (normalize-property-options property-options))
  (for/list ([overload (in-list overloads)])
    (define domain (property-overload-domain overload))
    (define range (property-overload-range overload))
    (define forge-name (overload-forge-name local domain range))
    (keyword-apply make-property-term
                   (map car normalized-options)
                   (map cdr normalized-options)
                   (list ont forge-name)
                   #:domain domain
                   #:range range
                   #:multiplicity (property-overload-multiplicity overload)
                   #:variable? (property-overload-variable? overload)
                   #:rdf-name local)))

(define (make-property-term/options ont local property-options)
  (define normalized-options (normalize-property-options property-options))
  (keyword-apply make-property-term
                 (map car normalized-options)
                 (map cdr normalized-options)
                 (list ont local)))

(define (ontology-property-matches ont local #:domain [domain #f] #:range [range #f])
  (filter (lambda (value)
            (and (term? value)
                 (eq? (term-kind value) 'property)
                 (eq? (term-rdf-name value) local)
                 (or (not domain)
                     (eq? (option-ref (term-options value) 'domain) domain))
                 (or (not range)
                     (eq? (option-ref (term-options value) 'range) range))))
          (ontology-declared-terms ont)))

(define (ontology-ensure-property ont local #:domain domain #:range range)
  (define matches (ontology-property-matches ont local #:domain domain #:range range))
  (case (length matches)
    [(1) (car matches)]
    [(0)
     (make-property-term ont (overload-forge-name local domain range)
                         #:domain domain
                         #:range range
                         #:rdf-name local)]
    [else (error 'ontology-ensure-property
                 "ambiguous property named ~a with requested domain/range"
                 local)]))

(define (ontology-declared-terms ont)
  (reverse (unbox (ontology-terms ont))))

(define (option-ref options key [fallback #f])
  (define found (assoc key options))
  (if found (cdr found) fallback))

(define (ontology-property ont local #:domain [domain #f] #:range [range #f])
  (define matches (ontology-property-matches ont local #:domain domain #:range range))
  (case (length matches)
    [(1) (car matches)]
    [(0) (error 'ontology-property "no property named ~a with requested domain/range" local)]
    [else (error 'ontology-property "ambiguous property named ~a with requested domain/range" local)]))

(define (term-forge-name value)
  (symbol->string (option-ref (term-options value) 'forge-name (term-local value))))

(define (term-rdf-name value)
  (option-ref (term-options value) 'rdf-name (term-local value)))

(define (term-iri value)
  (string-append (ontology-base (term-ontology value))
                 (symbol->string (term-rdf-name value))))

(define (turtle-name value)
  (format "~a:~a"
          (ontology-prefix (term-ontology value))
          (term-rdf-name value)))

(define (turtle-literal value)
  (cond
    [(term? value) (turtle-name value)]
    [(symbol? value) (format "~a" value)]
    [else (format "~s" value)]))

(define (turtle-local-name ont local)
  (format "~a:~a" (ontology-prefix ont) local))

(define (turtle-list items)
  (format "(~a)" (string-join items " ")))

(define (turtle-annotation-predicate ont value)
  (case value
    [(label) "rdfs:label"]
    [(comment) "rdfs:comment"]
    [(pref-label) "skos:prefLabel"]
    [(alt-label) "skos:altLabel"]
    [(definition) "skos:definition"]
    [(example) "skos:example"]
    [(elucidation) "skos:definition"]
    [(clif-label) (turtle-local-name ont 'bfo-clif-label)]
    [(fol-axiom) (turtle-local-name ont 'has-associated-axiom-fol)]
    [(nl-axiom) (turtle-local-name ont 'has-associated-axiom-nl)]
    [else
     (cond
       [(term? value) (turtle-name value)]
       [(symbol? value) (turtle-local-name ont value)]
       [else (format "~a" value)])]))

(define (annotations->turtle ont subject annotations)
  (apply string-append
         (for/list ([annotation (in-list annotations)])
           (format "~a ~a ~a .\n"
                   subject
                   (turtle-annotation-predicate ont (car annotation))
                   (turtle-literal (cdr annotation))))))

(define (logical-axiom-field annotations key [fallback #f])
  (define found (assoc key annotations))
  (if found (cdr found) fallback))

(define (logical-axiom-annotations->turtle ont label source annotations)
  (apply string-append
         (for/list ([annotation (in-list annotations)]
                    #:when (memq (car annotation) '(nl-axiom fol-axiom)))
           (define predicate (turtle-annotation-predicate ont (car annotation)))
           (define target (cdr annotation))
           (string-append
            (format "~a ~a ~a .\n" source predicate (turtle-literal target))
            (format "[ a owl:Axiom ; owl:annotatedSource ~a ; owl:annotatedProperty ~a ; owl:annotatedTarget ~a ; ~a ~a ] .\n"
                    source
                    predicate
                    (turtle-literal target)
                    (turtle-local-name ont 'has-axiom-label)
                    label)))))

(define (pairs items)
  (cond
    [(null? items) '()]
    [else
     (append (for/list ([right (in-list (cdr items))])
               (list (car items) right))
             (pairs (cdr items)))]))

(define (term->turtle value)
  (cond
    [(term? value)
     (define options (term-options value))
     (case (term-kind value)
       [(class)
        (string-append
         (format "~a a owl:Class .\n" (turtle-name value))
         (if (option-ref options 'subclass-of)
             (format "~a rdfs:subClassOf ~a .\n"
                     (turtle-name value)
                     (turtle-literal (option-ref options 'subclass-of)))
             "")
         (if (option-ref options 'variant-children)
             (format "~a owl:disjointUnionOf ~a .\n"
                     (turtle-name value)
                     (turtle-list
                      (map (lambda (local)
                             (turtle-local-name (term-ontology value) local))
                           (option-ref options 'variant-children))))
             "")
         (annotations->turtle
          (term-ontology value)
          (turtle-name value)
          (option-ref options 'annotations '())))]
       [(property)
        (define name (turtle-local-name (term-ontology value)
                                        (term-rdf-name value)))
        (string-append
         (format "~a a owl:ObjectProperty .\n" name)
         (if (option-ref options 'transitive?)
             (format "~a a owl:TransitiveProperty .\n" name)
             "")
         (if (option-ref options 'symmetric?)
             (format "~a a owl:SymmetricProperty .\n" name)
             "")
         (if (option-ref options 'asymmetric?)
             (format "~a a owl:AsymmetricProperty .\n" name)
             "")
         (if (option-ref options 'reflexive?)
             (format "~a a owl:ReflexiveProperty .\n" name)
             "")
         (if (option-ref options 'irreflexive?)
             (format "~a a owl:IrreflexiveProperty .\n" name)
             "")
         (if (option-ref options 'functional?)
             (format "~a a owl:FunctionalProperty .\n" name)
             "")
         (if (option-ref options 'inverse-functional?)
             (format "~a a owl:InverseFunctionalProperty .\n" name)
             "")
         (if (option-ref options 'subproperty-of)
             (format "~a rdfs:subPropertyOf ~a .\n"
                     name
                     (turtle-literal (option-ref options 'subproperty-of)))
             "")
         (if (option-ref options 'domain)
             (format "~a rdfs:domain ~a .\n"
                     name
                     (turtle-literal (option-ref options 'domain)))
             "")
         (if (option-ref options 'range)
             (format "~a rdfs:range ~a .\n"
                     name
                     (turtle-literal (option-ref options 'range)))
             "")
         (if (option-ref options 'inverse-of)
             (format "~a owl:inverseOf ~a .\n"
                     name
                     (turtle-literal (option-ref options 'inverse-of)))
             "")
         (annotations->turtle
          (term-ontology value)
          name
          (option-ref options 'annotations '())))]
       [else ""])]
    [(ontology-axiom? value)
     (case (ontology-axiom-kind value)
       [(disjoint)
        (apply string-append
               (for/list ([pair (in-list (pairs (ontology-axiom-args value)))])
                 (format "~a owl:disjointWith ~a .\n"
                         (turtle-literal (car pair))
                         (turtle-literal (cadr pair)))))]
       [(equivalent-union)
        (define args (ontology-axiom-args value))
        (format "~a owl:equivalentClass [ owl:unionOf ~a ] .\n"
                (turtle-literal (car args))
                (turtle-list (map turtle-literal (cdr args))))]
       [(equivalent-intersection)
        (define args (ontology-axiom-args value))
        (format "~a owl:equivalentClass [ owl:intersectionOf ~a ] .\n"
                (turtle-literal (car args))
                (turtle-list (map turtle-literal (cdr args))))]
       [(subclass-some)
        (define args (ontology-axiom-args value))
        (format "~a rdfs:subClassOf [ a owl:Restriction ; owl:onProperty ~a ; owl:someValuesFrom ~a ] .\n"
                (turtle-literal (list-ref args 0))
                (turtle-literal (list-ref args 1))
                (turtle-literal (list-ref args 2)))]
       [(subclass-only)
        (define args (ontology-axiom-args value))
        (format "~a rdfs:subClassOf [ a owl:Restriction ; owl:onProperty ~a ; owl:allValuesFrom ~a ] .\n"
                (turtle-literal (list-ref args 0))
                (turtle-literal (list-ref args 1))
                (turtle-literal (list-ref args 2)))]
       [(property-chain)
        (define args (ontology-axiom-args value))
        (format "~a owl:propertyChainAxiom ~a .\n"
                (turtle-literal (car args))
                (turtle-list (map turtle-literal (cdr args))))]
       [(logical-axiom)
        (define args (ontology-axiom-args value))
        (define label (turtle-local-name (ontology-axiom-ontology value)
                                         (list-ref args 0)))
        (define source (turtle-literal (list-ref args 1)))
        (define annotations (list-ref args 2))
        (string-append
         (format "~a a owl:NamedIndividual .\n" label)
         (annotations->turtle (ontology-axiom-ontology value)
                              label
                              (filter (lambda (annotation)
                                        (not (memq (car annotation)
                                                   '(nl-axiom fol-axiom))))
                                      annotations))
         (logical-axiom-annotations->turtle (ontology-axiom-ontology value)
                                            label
                                            source
                                            annotations))]
       [(annotation-property)
        (define local (list-ref (ontology-axiom-args value) 0))
        (define annotations (list-ref (ontology-axiom-args value) 1))
        (define subproperty (list-ref (ontology-axiom-args value) 2))
        (define name (turtle-local-name (ontology-axiom-ontology value) local))
        (string-append
         (format "~a a owl:AnnotationProperty .\n" name)
         (if subproperty
             (format "~a rdfs:subPropertyOf ~a .\n"
                     name
                     (turtle-annotation-predicate (ontology-axiom-ontology value)
                                                  subproperty))
             "")
         (annotations->turtle (ontology-axiom-ontology value)
                              name
                              annotations))]
       [else ""])]
    [else ""]))

(define (ontology->turtle ont)
  (string-append
   (format "@prefix ~a: <~a> .\n" (ontology-prefix ont) (ontology-base ont))
   "@prefix owl: <http://www.w3.org/2002/07/owl#> .\n"
   "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
   "@prefix skos: <http://www.w3.org/2004/02/skos/core#> .\n\n"
   (apply string-append (map term->turtle (ontology-declared-terms ont)))))

(define-syntax-rule (define-ontology name base)
  (define name (make-ontology 'name base)))

(define-syntax (define-class stx)
  (syntax-parse stx
    [(_ ont:id name:id)
     #'(define name (make-class-term ont 'name))]
    [(_ ont:id name:id (clause ...))
     (with-syntax ([(annotation ...)
                    (filter values
                            (map annotation-clause
                                 (syntax->list #'(clause ...))))])
       #'(define name
           (make-class-term ont 'name
                            #:annotations (list annotation ...))))]
    [(_ ont:id name:id #:abstract)
     #'(define name (make-class-term ont 'name #:abstract #t))]
    [(_ ont:id name:id #:subclass-of parent:id)
     #'(define name (make-class-term ont 'name #:subclass-of parent))]
    [(_ ont:id name:id #:subclass-of parent:id (clause ...))
     (with-syntax ([(annotation ...)
                    (filter values
                            (map annotation-clause
                                 (syntax->list #'(clause ...))))])
       #'(define name
           (make-class-term ont 'name
                            #:subclass-of parent
                            #:annotations (list annotation ...))))]
    [(_ ont:id name:id #:abstract #:subclass-of parent:id)
     #'(define name (make-class-term ont 'name #:abstract #t #:subclass-of parent))]
    [(_ ont:id name:id #:subclass-of parent:id #:abstract)
     #'(define name (make-class-term ont 'name #:abstract #t #:subclass-of parent))]))

(define-syntax (define-variant stx)
  (syntax-parse stx
    [(_ ont:id parent:id (child:id ...))
     #'(begin
         (define parent
           (make-class-term ont
                            'parent
                            #:abstract #t
                            #:variant-children '(child ...)))
         (define child (make-class-term ont 'child #:subclass-of parent))
         ...)]))

(define-for-syntax (annotation-clause stx)
  (define datum (syntax->datum stx))
  (cond
    [(and (pair? datum)
          (= (length datum) 2)
          (memq (car datum)
                '(label
                  comment
                  pref-label
                  alt-label
                  definition
                  example
                  elucidation
                  clif-label
                  fol-axiom
                  nl-axiom
                  annotation)))
     (if (eq? (car datum) 'annotation)
         #f
         #`(cons '#,(car datum) #,(cadr (syntax->list stx))))]
    [(and (pair? datum)
          (= (length datum) 3)
          (eq? (car datum) 'annotation)
          (symbol? (cadr datum)))
     #`(cons '#,(cadr datum) #,(caddr (syntax->list stx)))]
    [else #f]))

(define-for-syntax (annotation-property-subproperty-clause stx)
  (define datum (syntax->datum stx))
  (and (pair? datum)
       (= (length datum) 2)
       (eq? (car datum) 'subproperty-of)
       (symbol? (cadr datum))
       #`'#,(cadr datum)))

(define-for-syntax (annotation-property-subproperty clauses-stx)
  (define matches
    (filter values
            (map annotation-property-subproperty-clause
                 (syntax->list clauses-stx))))
  (cond
    [(null? matches) #'#f]
    [(null? (cdr matches)) (car matches)]
    [else
     (raise-syntax-error 'annotation-property
                         "expected at most one subproperty-of clause"
                         clauses-stx)]))

(define-syntax (define-disjoint stx)
  (syntax-parse stx
    [(_ ont:id class:id ...)
     #'(void (make-axiom ont 'disjoint (list class ...)))]))

(define-syntax (define-equivalent-union stx)
  (syntax-parse stx
    [(_ ont:id class:id member:id ...+)
     #'(void (make-axiom ont 'equivalent-union (list class member ...)))]))

(define-syntax (define-equivalent-intersection stx)
  (syntax-parse stx
    [(_ ont:id class:id member:id ...+)
     #'(void (make-axiom ont 'equivalent-intersection (list class member ...)))]))

(define-syntax (define-subclass-some stx)
  (syntax-parse stx
    [(_ ont:id class:id property:id filler:id)
     #'(void (make-axiom ont 'subclass-some (list class property filler)))]))

(define-syntax (define-subclass-only stx)
  (syntax-parse stx
    [(_ ont:id class:id property:id filler:id)
     #'(void (make-axiom ont 'subclass-only (list class property filler)))]))

(define-syntax (define-property-chain stx)
  (syntax-parse stx
    [(_ ont:id property:id (step:id ...+))
     #'(void (make-axiom ont 'property-chain (list property step ...)))]
    [(_ ont:id property:id step:id ...+)
     #'(void (make-axiom ont 'property-chain (list property step ...)))]))

(define-for-syntax (logical-axiom-clause stx)
  (define datum (syntax->datum stx))
  (cond
    [(and (pair? datum)
          (= (length datum) 2)
          (memq (car datum) '(label comment nl fol fol-axiom nl-axiom)))
     (define key
       (case (car datum)
         [(nl) 'nl-axiom]
         [(fol) 'fol-axiom]
         [else (car datum)]))
     #`(cons '#,key #,(cadr (syntax->list stx)))]
    [else #f]))

(define-syntax (define-logical-axiom stx)
  (syntax-parse stx
    [(_ ont:id name:id subject:id (clause ...))
     (with-syntax ([(annotation ...)
                    (filter values
                            (map logical-axiom-clause
                                 (syntax->list #'(clause ...))))])
       #'(void (make-axiom ont
                           'logical-axiom
                           (list 'name subject (list annotation ...)))))]))

(define-syntax (define-annotation-property stx)
  (syntax-parse stx
    [(_ ont:id name:id)
     #'(void (make-axiom ont 'annotation-property (list 'name '() #f)))]
    [(_ ont:id name:id (clause ...))
     (with-syntax ([(annotation ...)
                    (filter values
                            (map annotation-clause
                                 (syntax->list #'(clause ...))))]
                   [subproperty
                    (annotation-property-subproperty #'(clause ...))])
       #'(void (make-axiom ont
                           'annotation-property
                           (list 'name (list annotation ...) subproperty))))]))

(begin-for-syntax
  (define property-option-heads
    '(inverse-of
      subproperty-of
      transitive
      symmetric
      asymmetric
      reflexive
      irreflexive
      functional
      inverse-functional))

  (define (property-option-head? value)
    (and (symbol? value) (memq value property-option-heads)))

  (define-syntax-class property-clause
    #:attributes (domain range multiplicity variable?)
    [pattern (domain:id range:id)
     #:fail-when (property-option-head? (syntax-e #'domain))
     "expected a property domain class, not an OWL property axiom keyword"
     #:attr multiplicity #''set
     #:attr variable? #'#f]
    [pattern (domain:id (~datum one) range:id)
     #:fail-when (property-option-head? (syntax-e #'domain))
     "expected a property domain class, not an OWL property axiom keyword"
     #:attr multiplicity #''one
     #:attr variable? #'#f]
    [pattern (domain:id (~datum lone) range:id)
     #:fail-when (property-option-head? (syntax-e #'domain))
     "expected a property domain class, not an OWL property axiom keyword"
     #:attr multiplicity #''lone
     #:attr variable? #'#f]
    [pattern (domain:id (~datum set) range:id)
     #:fail-when (property-option-head? (syntax-e #'domain))
     "expected a property domain class, not an OWL property axiom keyword"
     #:attr multiplicity #''set
     #:attr variable? #'#f]
    [pattern (domain:id (~datum var) (~datum one) range:id)
     #:fail-when (property-option-head? (syntax-e #'domain))
     "expected a property domain class, not an OWL property axiom keyword"
     #:attr multiplicity #''one
     #:attr variable? #'#t]
    [pattern (domain:id (~datum var) (~datum lone) range:id)
     #:fail-when (property-option-head? (syntax-e #'domain))
     "expected a property domain class, not an OWL property axiom keyword"
     #:attr multiplicity #''lone
     #:attr variable? #'#t]
    [pattern (domain:id (~datum var) (~datum set) range:id)
     #:fail-when (property-option-head? (syntax-e #'domain))
     "expected a property domain class, not an OWL property axiom keyword"
     #:attr multiplicity #''set
     #:attr variable? #'#t]))

(define-for-syntax (property-option-clause stx)
  (define datum (syntax->datum stx))
  (case datum
    [(transitive) #'(cons '#:transitive? #t)]
    [(symmetric) #'(cons '#:symmetric? #t)]
    [(asymmetric) #'(cons '#:asymmetric? #t)]
    [(reflexive) #'(cons '#:reflexive? #t)]
    [(irreflexive) #'(cons '#:irreflexive? #t)]
    [(functional) #'(cons '#:functional? #t)]
    [(inverse-functional) #'(cons '#:inverse-functional? #t)]
    [else
     (cond
       [(and (pair? datum) (eq? (car datum) 'inverse-of) (= (length datum) 2))
        #`(cons '#:inverse-of #,(cadr (syntax->list stx)))]
       [(and (pair? datum) (eq? (car datum) 'subproperty-of) (= (length datum) 2))
        #`(cons '#:subproperty-of #,(cadr (syntax->list stx)))]
       [(and (pair? datum) (= (length datum) 1))
        (property-option-clause (car (syntax->list stx)))]
       [else #f])]))

(define-for-syntax (property-domain-clause? stx)
  (syntax-parse stx
    [clause:property-clause #t]
    [_ #f]))

(define-for-syntax (property-clauses->options clauses-stx)
  (filter values (map property-option-clause (syntax->list clauses-stx))))

(define-for-syntax (property-clauses->domains clauses-stx)
  (filter property-domain-clause? (syntax->list clauses-stx)))

(define-syntax (define-property stx)
  (syntax-parse stx
    [(_ ont:id name:id)
     #'(begin
         (void (make-property-term ont 'name))
         (define-syntax (name use-stx)
           (syntax-parse use-stx
             [id:id
              #'(ontology-property ont 'name)]
             [(_ (range*:id domain*:id))
              #'(ontology-ensure-property ont 'name #:domain domain* #:range range*)]
             [(_ range*:id)
              #'(ontology-property ont 'name #:range range*)])))]
    [(_ ont:id name:id (clause:property-clause ...))
     #'(begin
         (void (make-property-family
                ont
                'name
                (list (list clause.domain
                            clause.range
                            clause.multiplicity
                            clause.variable?)
                      ...)))
         (define-syntax (name use-stx)
           (syntax-parse use-stx
             [id:id
              #'(ontology-property ont 'name)]
             [(_ (range*:id domain*:id))
              #'(ontology-ensure-property ont 'name #:domain domain* #:range range*)]
             [(_ range*:id)
              #'(ontology-property ont 'name #:range range*)])))]
    [(_ ont:id name:id (clause ...))
     (define domain-clauses (property-clauses->domains #'(clause ...)))
     (define option-clauses (property-clauses->options #'(clause ...)))
     (define annotation-clauses
       (filter values (map annotation-clause (syntax->list #'(clause ...)))))
     (cond
       [(null? domain-clauses)
        (with-syntax ([(option-clause ...) option-clauses]
                      [(annotation-clause ...) annotation-clauses])
          #'(begin
              (void
               (make-property-term/options
                ont
                'name
                (list option-clause ...
                      (cons '#:annotations
                            (list annotation-clause ...)))))
              (define-syntax (name use-stx)
                (syntax-parse use-stx
                  [id:id
                   #'(ontology-property ont 'name)]
                  [(_ (range*:id domain*:id))
                   #'(ontology-ensure-property ont 'name #:domain domain* #:range range*)]
                  [(_ range*:id)
                   #'(ontology-property ont 'name #:range range*)]))))]
       [else
        (with-syntax ([(domain-clause ...) domain-clauses]
                      [(option-clause ...) option-clauses]
                      [(annotation-clause ...) annotation-clauses])
          #'(define-property/mixed ont name
              (domain-clause ...)
              (option-clause ...)
              (annotation-clause ...)))])]
    [(_ ont:id name:id option ...)
     #'(define-property/options ont name (option ...))]
    [(_ ont:id binding:id #:name local:id #:domain domain:id #:range range:id)
     #'(define binding (make-property-term ont 'local
                                           #:domain domain
                                           #:range range
                                           #:forge-name 'binding))]
    [(_ ont:id binding:id #:name local:id #:domain domain:id #:range range:id #:inverse-of inverse:id)
     #'(define binding (make-property-term ont 'local
                                           #:domain domain
                                           #:range range
                                           #:inverse-of inverse
                                           #:forge-name 'binding))]
    [(_ ont:id name:id #:domain domain:id #:range range:id)
     #'(define name (make-property-term ont 'name #:domain domain #:range range))]
    [(_ ont:id name:id #:domain domain:id #:range range:id #:inverse-of inverse:id)
     #'(define name (make-property-term ont 'name #:domain domain #:range range #:inverse-of inverse))]))

(define-syntax (define-property/options stx)
  (syntax-parse stx
    [(_ ont:id name:id option ...)
     #'(begin
         (void (make-property-term ont 'name option ...))
         (define-syntax (name use-stx)
           (syntax-parse use-stx
             [id:id
              #'(ontology-property ont 'name)]
             [(_ (range*:id domain*:id))
              #'(ontology-ensure-property ont 'name #:domain domain* #:range range*)]
             [(_ range*:id)
              #'(ontology-property ont 'name #:range range*)])))]))

(define-syntax (define-property/mixed stx)
  (syntax-parse stx
    [(_ ont:id name:id (clause:property-clause ...) (option-clause ...) (annotation-clause ...))
     #'(begin
         (void (make-property-family
                ont
                'name
                (list (list clause.domain
                            clause.range
                            clause.multiplicity
                            clause.variable?)
                      ...)
                #:options (list option-clause ...
                                (cons '#:annotations
                                      (list annotation-clause ...)))))
         (define-syntax (name use-stx)
           (syntax-parse use-stx
             [id:id
              #'(ontology-property ont 'name)]
             [(_ (range*:id domain*:id))
              #'(ontology-ensure-property ont 'name #:domain domain* #:range range*)]
             [(_ range*:id)
              #'(ontology-property ont 'name #:range range*)])))]))
