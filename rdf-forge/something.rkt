#lang racket/base

(require (for-syntax racket/base
                     racket/list
                     racket/match
                     syntax/parse)
         (only-in something/base #%rewrite-infix)
         (prefix-in o: "ontology.rkt")
         (except-in "model.rkt"
                    model
                    option
                    signature
                    predicate
                    run
                    check
                    all
                    some
                    lone
                    always
                    next-state
                    not
                    block)
         (prefix-in f: "model.rkt"))

(provide (all-from-out "ontology.rkt")
         ontology
         class
         variant
         disjoint
         equivalent-union
         equivalent-intersection
         subclass-some
         subclass-only
         property-chain
         logical-axiom
         annotation-property
         property
         inverse-of
         subproperty-of
         transitive
         symmetric
         asymmetric
         reflexive
         irreflexive
         functional
         inverse-functional
         option
         field
         one
         no
         (rename-out [forge-not not])
         =>
         &&
         ||
         either
         conjunct
         disjunct
         conjunct-prefix
         disjunct-prefix
         (rename-out [forge-block block])
         ==
         in
         join
         rjoin
         follow
         matching
         union
         intersect
         count
         ge
         prime
         model->forge-runs
         run-forge-model
         check-forge-model
         write-forge-run-xml
         (rename-out [forge-model model]
                     [forge-signature signature]
                     [forge-predicate predicate]
                     [forge-run run]
                     [forge-check check]
                     [forge-all all]
                     [forge-forall forall]
                     [forge-some some]
                     [forge-exists exists]
                     [forge-lone lone]
                     [forge-always always]
                     [forge-next-state next-state]))

(define either f:||)
(define forge-and f:&&)
(define forge-or f:||)
(define forge-not f:not)

(begin-for-syntax
  (struct boolean-node (marker children) #:transparent)

  (define (boolean-marker stx)
    (case (syntax-e stx)
      [(&& |/\| conjunct) 'conjunct]
      [(\|\| |\/| disjunct) 'disjunct]
      [else #f]))

  (define (marker-head marker)
    (case marker
      [(conjunct) #'forge-and]
      [(disjunct) #'forge-or]
      [else (raise-argument-error 'marker-head "boolean marker" marker)]))

  (define (trailing-block pieces)
    (and (pair? pieces)
         (let ([block-pieces (syntax->list (last pieces))])
           (and block-pieces
                (pair? block-pieces)
                (eq? (syntax-e (car block-pieces)) 'block)
                (datum->syntax (last pieces)
                               (cdr block-pieces)
                               (last pieces)
                               (last pieces))))))

  (define (drop-trailing-block pieces)
    (if (trailing-block pieces)
        (drop-right pieces 1)
        pieces))

  (define (syntax-list* ctx pieces)
    (datum->syntax ctx pieces ctx ctx))

  (define (rewrite-infix-stx ctx pieces)
    #`(#%rewrite-infix #,(syntax-list* ctx pieces)))

  (define (line->boolean-node ctx pieces #:rewrite [rewrite rewrite-infix-stx])
    (let loop ([markers '()]
               [remaining pieces])
      (cond
        [(and (pair? remaining) (boolean-marker (car remaining)))
         (loop (append markers (list (boolean-marker (car remaining))))
               (cdr remaining))]
        [else
         (define children (trailing-block remaining))
         (define expr-pieces (drop-trailing-block remaining))
         (define leaf (rewrite ctx expr-pieces))
         (define node
           (for/fold ([inner leaf]) ([marker (in-list (reverse markers))])
             (boolean-node marker (list inner))))
         (if children
             (for/fold ([current node])
                       ([child (in-list (syntax->list children))])
               (insert-boolean-child
                current
                (line->boolean-node child (syntax->list child) #:rewrite rewrite)))
             node)])))

  (define (insert-boolean-child node child)
    (define target (and (boolean-node? child) (boolean-node-marker child)))
    (define-values (updated inserted?) (insert-boolean-child* node target child))
    (if inserted?
        updated
        (if (boolean-node? node)
            (boolean-node (boolean-node-marker node)
                          (append (boolean-node-children node) (list child)))
            child)))

  (define (insert-boolean-child* node target child)
    (cond
      [(not (boolean-node? node)) (values node #f)]
      [else
       (let loop ([seen '()]
                  [remaining (reverse (boolean-node-children node))])
         (cond
           [(null? remaining)
            (if (eq? (boolean-node-marker node) target)
                (values (boolean-node (boolean-node-marker node)
                                      (append (boolean-node-children node) (list child)))
                        #t)
                (values node #f))]
           [else
            (define-values (updated-child inserted?)
              (insert-boolean-child* (car remaining) target child))
            (if inserted?
                (values (boolean-node (boolean-node-marker node)
                                      (append (reverse (cdr remaining))
                                              (list updated-child)
                                              seen))
                        #t)
                (loop (cons (car remaining) seen) (cdr remaining)))]))]))

  (define (boolean-node->syntax node)
    (cond
      [(boolean-node? node)
       (with-syntax ([head (marker-head (boolean-node-marker node))]
                     [(child ...) (map boolean-node->syntax (boolean-node-children node))])
         #'(head child ...))]
      [else node]))

  (define (lamport-boolean-chain? stx)
    (define pieces (syntax->list stx))
    (and pieces
         (pair? pieces)
         (boolean-marker (car pieces))
         (pair? (cdr pieces))
         (boolean-marker (cadr pieces))))

  (define (rewrite-forge-block-clause clause)
    (if (lamport-boolean-chain? clause)
        (boolean-node->syntax (line->boolean-node clause (syntax->list clause)))
        #`(#%rewrite-infix #,clause)))

  (define (marked-clause marker stx)
    (syntax-parse stx
      [(head body)
       #:when (eq? (syntax-e #'head) marker)
       #'body]
      [_ #f]))

  (define (marked-block body-stx)
    (define body (syntax->list body-stx))
    (define conjuncts (map (lambda (clause) (marked-clause 'conjunct clause)) body))
    (define disjuncts (map (lambda (clause) (marked-clause 'disjunct clause)) body))
    (cond
      [(and (pair? conjuncts) (andmap values conjuncts))
       #`(f:&& #,@conjuncts)]
      [(and (pair? disjuncts) (andmap values disjuncts))
       #`(f:|| #,@disjuncts)]
      [else
       #`(f:block #,@body)])))

(define-syntax (forge-block stx)
  (syntax-parse stx
    [(_ body ...)
     #`(f:block #,@(map rewrite-forge-block-clause (syntax->list #'(body ...))))]))

(define-syntax (conjunct stx)
  (syntax-parse stx
    [(_ ((~datum block) body ...))
     (marked-block #'(body ...))]
    [(_ body)
     #'body]))

(define-syntax (disjunct stx)
  (syntax-parse stx
    [(_ ((~datum block) body ...))
     (marked-block #'(body ...))]
    [(_ body)
     #'body]))

(define-syntax (conjunct-prefix stx parse)
  (define rest (cdr (syntax->list stx)))
  (if (and (pair? rest) (boolean-marker (car rest)))
      (boolean-node->syntax
       (line->boolean-node stx
                           (cons #'&& rest)
                           #:rewrite (lambda (ctx pieces)
                                       (parse (datum->syntax ctx pieces ctx ctx)))))
      (with-syntax ([body (parse (datum->syntax stx rest stx stx))])
        #'(forge-and body))))

(define-syntax (disjunct-prefix stx parse)
  (define rest (cdr (syntax->list stx)))
  (if (and (pair? rest) (boolean-marker (car rest)))
      (boolean-node->syntax
       (line->boolean-node stx
                           (cons #'\|\| rest)
                           #:rewrite (lambda (ctx pieces)
                                       (parse (datum->syntax ctx pieces ctx ctx)))))
      (with-syntax ([body (parse (datum->syntax stx rest stx stx))])
        #'(forge-or body))))

(define-for-syntax (runtime-ref stx)
  (syntax-parse stx
    [name:id
     #''name]
    [((~datum always) body)
     #`(f:always #,(runtime-ref #'body))]
    [((~datum block) body ...)
     #`(f:block #,@(map runtime-ref (syntax->list #'(body ...))))]
    [other
     #'other]))

(define-for-syntax (scope-ref stx)
  (define datum (syntax->datum stx))
  (define (unwrap-seq value)
    (if (and (list? value)
             (= (length value) 2)
             (eq? (car value) '#%seq))
        (cadr value)
        value))
  (define groups
    (and (list? datum) (map unwrap-seq datum)))
  (define (scope-group? group)
    (and (list? group)
         (pair? group)
         (exact-nonnegative-integer? (car group))
         (andmap symbol? (cdr group))))
  (cond
    [(and groups (andmap scope-group? groups))
     (define compiled
       (apply append
              (for/list ([group (in-list groups)])
                (define count (car group))
                (for/list ([sig (in-list (cdr group))])
                  (list sig count count)))))
     (datum->syntax stx `(quote ,compiled))]
    [else
     stx]))

(define-for-syntax (ontology-clause-binding stx)
  (syntax-parse stx
    [((~datum class) name:id (~datum is) first:id (~seq (~datum or) rest:id) ...)
     (cons #'name (cons #'first (syntax->list #'(rest ...))))]
    [((~datum class) name:id option ...)
     (list #'name)]
    [((~datum variant) parent:id (child:id ...))
     (cons #'parent (syntax->list #'(child ...)))]
    [((~datum disjoint) class:id ...)
     '()]
    [((~datum equivalent-union) class:id member:id ...)
     '()]
    [((~datum equivalent-intersection) class:id member:id ...)
     '()]
    [((~datum subclass-some) class:id property:id filler:id)
     '()]
    [((~datum subclass-only) class:id property:id filler:id)
     '()]
    [((~datum property-chain) property:id (step:id ...))
     '()]
    [((~datum property-chain) property:id step:id ...)
     '()]
    [((~datum logical-axiom) name:id subject:id option ...)
     '()]
    [((~datum annotation-property) name:id option ...)
     '()]
    [((~datum property) name:id option ...)
     (list #'name)]
    [((~datum a) domain:id relation:id (~datum some) range:id)
     (list #'relation)]
    [((~datum a) domain:id relation:id (~datum one) range:id)
     (list #'relation)]
    [((~datum a) domain:id (~datum varyingly) relation:id
      (~datum some) range:id)
     (list #'relation)]
    [((~datum a) domain:id (~datum varyingly) relation:id
      (~datum one) range:id)
     (list #'relation)]
    [((~datum a) domain:id (~datum varyingly) relation:id
      (~datum a) range:id (~datum or) (~datum not))
     (list #'relation)]))

(define-for-syntax (property-clause-option? stx)
  (define datum (syntax->datum stx))
  (or (memq datum
            '(transitive
              symmetric
              asymmetric
              reflexive
              irreflexive
              functional
              inverse-functional))
      (and (pair? datum)
           (memq (car datum) '(inverse-of subproperty-of))
           (= (length datum) 2)
           (symbol? (cadr datum)))))

(define-for-syntax (annotation-clause? stx)
  (define datum (syntax->datum stx))
  (or (and (pair? datum)
           (= (length datum) 2)
           (memq (car datum)
                 '(label
                   comment
                   pref-label
                   alt-label
                   definition
                   example
                   clif-label
                   fol-axiom
                   nl-axiom
                   elucidation)))
      (and (pair? datum)
           (= (length datum) 3)
           (eq? (car datum) 'annotation)
           (symbol? (cadr datum)))))

(define-for-syntax (annotation-property-clause? stx)
  (define datum (syntax->datum stx))
  (or (annotation-clause? stx)
      (and (pair? datum)
           (= (length datum) 2)
           (eq? (car datum) 'subproperty-of)
           (symbol? (cadr datum)))))

(define-for-syntax (annotation-property-clauses? clauses-stx)
  (andmap annotation-property-clause? (syntax->list clauses-stx)))

(define-for-syntax (logical-axiom-clause? stx)
  (define datum (syntax->datum stx))
  (and (pair? datum)
       (= (length datum) 2)
       (memq (car datum) '(label comment nl fol nl-axiom fol-axiom))))

(define-for-syntax (logical-axiom-clauses? clauses-stx)
  (andmap logical-axiom-clause? (syntax->list clauses-stx)))

(define-for-syntax (property-clause-domain? stx)
  (define datum (syntax->datum stx))
  (and (list? datum)
       (not (and (pair? datum)
                 (memq (car datum)
                       '(inverse-of
                         subproperty-of
                         transitive
                         symmetric
                         asymmetric
                         reflexive
                         irreflexive
                         functional
                         inverse-functional))))
       (or (and (= (length datum) 2)
                (andmap symbol? datum))
           (and (= (length datum) 3)
                (symbol? (first datum))
                (memq (second datum) '(one lone set))
                (symbol? (third datum)))
           (and (= (length datum) 4)
                (symbol? (first datum))
                (eq? (second datum) 'var)
                (memq (third datum) '(one lone set))
                (symbol? (fourth datum))))))

(define-for-syntax (property-block-clause? stx)
  (or (property-clause-option? stx)
      (annotation-clause? stx)
      (property-clause-domain? stx)))

(define-for-syntax (property-block-clauses? clauses-stx)
  (andmap property-block-clause? (syntax->list clauses-stx)))

(define-syntax (ontology stx)
  (syntax-parse stx
    [(_ name:id base:expr ((~datum block) clause ...))
     (define clause-bindings
       (apply append
              (map ontology-clause-binding
                   (syntax->list #'(clause ...)))))
     #`(begin
         (o:define-ontology name base)
         (ontology-clause name clause) ...
         (provide name #,@clause-bindings))]))

(define-syntax (ontology-clause stx)
  (syntax-parse stx
    [(_ ont:id ((~datum class) name:id (~datum is) first:id (~seq (~datum or) rest:id) ...))
     #'(o:define-variant ont name (first rest ...))]
    [(_ ont:id ((~datum class) name:id))
     #'(o:define-class ont name)]
    [(_ ont:id ((~datum class) name:id ((~datum block) clause ...)))
     #'(o:define-class ont name (clause ...))]
    [(_ ont:id ((~datum class) name:id #:subclass-of parent:id ((~datum block) clause ...)))
     #'(o:define-class ont name #:subclass-of parent (clause ...))]
    [(_ ont:id ((~datum class) name:id option ...))
     #'(o:define-class ont name option ...)]
    [(_ ont:id ((~datum variant) parent:id (child:id ...)))
     #'(o:define-variant ont parent (child ...))]
    [(_ ont:id ((~datum disjoint) class:id ...))
     #'(o:define-disjoint ont class ...)]
    [(_ ont:id ((~datum equivalent-union) class:id member:id ...))
     #'(o:define-equivalent-union ont class member ...)]
    [(_ ont:id ((~datum equivalent-intersection) class:id member:id ...))
     #'(o:define-equivalent-intersection ont class member ...)]
    [(_ ont:id ((~datum subclass-some) class:id property:id filler:id))
     #'(o:define-subclass-some ont class property filler)]
    [(_ ont:id ((~datum subclass-only) class:id property:id filler:id))
     #'(o:define-subclass-only ont class property filler)]
    [(_ ont:id ((~datum property-chain) property:id (step:id ...)))
     #'(o:define-property-chain ont property (step ...))]
    [(_ ont:id ((~datum property-chain) property:id step:id ...))
     #'(o:define-property-chain ont property step ...)]
    [(_ ont:id ((~datum logical-axiom) name:id subject:id ((~datum block) clause ...)))
     #:when (logical-axiom-clauses? #'(clause ...))
     #'(o:define-logical-axiom ont name subject (clause ...))]
    [(_ ont:id ((~datum annotation-property) name:id ((~datum block) clause ...)))
     #:when (annotation-property-clauses? #'(clause ...))
     #'(o:define-annotation-property ont name (clause ...))]
    [(_ ont:id ((~datum annotation-property) name:id))
     #'(o:define-annotation-property ont name)]
    [(_ ont:id ((~datum property) name:id ((~datum block) clause ...)))
     #:when (property-block-clauses? #'(clause ...))
     #'(o:define-property ont name (clause ...))]
    [(_ ont:id ((~datum property) name:id ((~datum block))))
     #'(o:define-property ont name)]
    [(_ ont:id ((~datum property) name:id option:keyword value ...))
     #'(o:define-property ont name option value ...)]
    [(_ ont:id ((~datum property) name:id))
     #'(o:define-property ont name)]
    [(_ ont:id ((~datum property) name:id option ...))
     #'(o:define-property ont name option ...)]
    [(_ ont:id ((~datum property) name:id (domain:id range:id) ...))
     #'(o:define-property ont name ((domain range) ...))]
    [(_ ont:id ((~datum a) domain:id relation:id (~datum some) range:id))
     #'(o:define-property ont relation ((domain set range)))]
    [(_ ont:id ((~datum a) domain:id relation:id (~datum one) range:id))
     #'(o:define-property ont relation ((domain one range)))]
    [(_ ont:id ((~datum a) domain:id (~datum varyingly) relation:id
                (~datum some) range:id))
     #'(o:define-property ont relation ((domain var set range)))]
    [(_ ont:id ((~datum a) domain:id (~datum varyingly) relation:id
                (~datum one) range:id))
     #'(o:define-property ont relation ((domain var one range)))]
    [(_ ont:id ((~datum a) domain:id (~datum varyingly) relation:id
                (~datum a) range:id (~datum or) (~datum not)))
     #'(o:define-property ont relation ((domain var lone range)))]))

(define-syntax (class stx)
  (raise-syntax-error 'class "class is only valid inside an ontology block" stx))

(define-syntax (variant stx)
  (raise-syntax-error 'variant "variant is only valid inside an ontology block" stx))

(define-syntax (disjoint stx)
  (raise-syntax-error 'disjoint "disjoint is only valid inside an ontology block" stx))

(define-syntax (equivalent-union stx)
  (raise-syntax-error 'equivalent-union "equivalent-union is only valid inside an ontology block" stx))

(define-syntax (equivalent-intersection stx)
  (raise-syntax-error 'equivalent-intersection "equivalent-intersection is only valid inside an ontology block" stx))

(define-syntax (subclass-some stx)
  (raise-syntax-error 'subclass-some "subclass-some is only valid inside an ontology block" stx))

(define-syntax (subclass-only stx)
  (raise-syntax-error 'subclass-only "subclass-only is only valid inside an ontology block" stx))

(define-syntax (property-chain stx)
  (raise-syntax-error 'property-chain "property-chain is only valid inside an ontology block" stx))

(define-syntax (logical-axiom stx)
  (raise-syntax-error 'logical-axiom "logical-axiom is only valid inside an ontology block" stx))

(define-syntax (annotation-property stx)
  (raise-syntax-error 'annotation-property "annotation-property is only valid inside an ontology block" stx))

(define-syntax (property stx)
  (raise-syntax-error 'property "property is only valid inside an ontology block" stx))

(define-syntax (inverse-of stx)
  (raise-syntax-error 'inverse-of "inverse-of is only valid inside a property block" stx))

(define-syntax (subproperty-of stx)
  (raise-syntax-error 'subproperty-of "subproperty-of is only valid inside a property block" stx))

(define-syntax (transitive stx)
  (raise-syntax-error 'transitive "transitive is only valid inside a property block" stx))

(define-syntax (symmetric stx)
  (raise-syntax-error 'symmetric "symmetric is only valid inside a property block" stx))

(define-syntax (asymmetric stx)
  (raise-syntax-error 'asymmetric "asymmetric is only valid inside a property block" stx))

(define-syntax (reflexive stx)
  (raise-syntax-error 'reflexive "reflexive is only valid inside a property block" stx))

(define-syntax (irreflexive stx)
  (raise-syntax-error 'irreflexive "irreflexive is only valid inside a property block" stx))

(define-syntax (functional stx)
  (raise-syntax-error 'functional "functional is only valid inside a property block" stx))

(define-syntax (inverse-functional stx)
  (raise-syntax-error 'inverse-functional "inverse-functional is only valid inside a property block" stx))

(define-syntax (option stx)
  (syntax-parse stx
    [(_ name:id value:expr)
     #'(f:option 'name value)]
    [(_ name:expr value:expr)
     #'(f:option name value)]))

(define-syntax (forge-body stx)
  (syntax-parse stx
    [(_ ((~datum block) body ...))
     #`(f:block #,@(map rewrite-forge-block-clause (syntax->list #'(body ...))))]
    [(_ body:expr)
     #'body]))

(define-syntax (forge-model stx)
  (syntax-parse stx
    [(_ name:id
        (~optional (~seq #:language language:expr)
                   #:defaults ([language #''forge/temporal]))
        ((~datum block) part:expr ...))
     #'(begin
         (define name (f:model #:language language part ...))
         (provide name))]
    [(_ (~optional (~seq #:language language:expr)
                   #:defaults ([language #''forge/temporal]))
        ((~datum block) part:expr ...))
     #'(f:model #:language language part ...)]
    [(_ (~optional (~seq #:language language:expr)
                   #:defaults ([language #''forge/temporal]))
        part:expr ...)
     #'(f:model #:language language part ...)]))

(define-syntax (forge-signature stx)
  (syntax-parse stx
    [(_ term:expr)
     #'(f:signature term)]
    [(_ term:expr ((~datum block) field-clause ...))
     #'(f:signature term field-clause ...)]
    [(_ term:expr field-clause ...)
     #'(f:signature term field-clause ...)]))

(define-syntax (forge-predicate stx)
  (syntax-parse stx
    [(_ name:id body)
     #'(f:predicate 'name (forge-body body))]
    [(_ name:expr body)
     #'(f:predicate name (forge-body body))]))

(begin-for-syntax
  (define (run-scope-piece->symbol piece)
    (match piece
      [(? symbol?) piece]
      [(list 'unquote (? symbol? sig)) sig]
      [_
       (raise-argument-error 'run-scope-piece->symbol
                             "scope signature identifier"
                             piece)]))

  (define (run-scope-clause->entries stx)
    (match (syntax->datum stx)
      [`(exactly ,(? exact-nonnegative-integer? count) of ,pieces ...)
       (datum->syntax stx
                      (for/list ([sig (in-list (map run-scope-piece->symbol pieces))])
                        (list sig count count))
                      stx)]
      [_
       (raise-syntax-error #f
                           "expected a scope clause like `exactly 1 of A, B`"
                           stx)]))

  (define (run-scope-clauses->scope entries-stx)
    (define entries
      (apply append (map syntax->datum (syntax->list entries-stx))))
    (datum->syntax entries-stx `(quote ,entries) entries-stx))

  (define-syntax-class run-scope-clause
    #:attributes (entries)
    [pattern ((~datum exactly) part ...)
     #:attr entries (run-scope-clause->entries #'(exactly part ...))])

  (define-syntax-class run-steps-clause
    #:attributes (trace-length)
    [pattern ((~datum for) trace-length:number (~datum steps))
     #:fail-unless (exact-positive-integer? (syntax-e #'trace-length))
     "expected an exact positive step count"])

  (define-syntax-class run-body-clause
    #:attributes (expr)
    [pattern ((~datum show) body)
     #:attr expr (runtime-ref #'body)]
    [pattern ((~datum always) body)
     #:attr expr (runtime-ref #'(always body))]
    [pattern body
     #:attr expr (runtime-ref #'body)]))

(define-syntax (forge-run stx)
  (syntax-parse stx
    [(_ name:id
        ((~datum block)
         scope:run-scope-clause ...+
         steps:run-steps-clause
         body:run-body-clause ...))
     #`(f:run 'name
              (f:block body.expr ...)
              #:for #,(run-scope-clauses->scope #'(scope.entries ...))
              #:min-tracelength steps.trace-length
              #:max-tracelength steps.trace-length)]
    [(_ name:id
        ((~datum block)
         scope:run-scope-clause ...+
         body:run-body-clause ...))
     #`(f:run 'name
              (f:block body.expr ...)
              #:for #,(run-scope-clauses->scope #'(scope.entries ...)))]
    [(_ name:id
        scope:run-scope-clause ...+
        steps:run-steps-clause
        body:run-body-clause ...)
     #`(f:run 'name
              (f:block body.expr ...)
              #:for #,(run-scope-clauses->scope #'(scope.entries ...))
              #:min-tracelength steps.trace-length
              #:max-tracelength steps.trace-length)]
    [(_ name:id
        scope:run-scope-clause ...+
        body:run-body-clause ...)
     #`(f:run 'name
              (f:block body.expr ...)
              #:for #,(run-scope-clauses->scope #'(scope.entries ...)))]
    [(_ name:id #:for scope #:trace-length trace-length:expr body)
     #`(f:run 'name #,(runtime-ref #'body)
              #:for #,(scope-ref #'scope)
              #:min-tracelength trace-length
              #:max-tracelength trace-length)]
    [(_ name:id body #:for scope)
     #`(f:run 'name (forge-body body) #:for #,(scope-ref #'scope))]
    [(_ name:id #:for scope body)
     #`(f:run 'name #,(runtime-ref #'body) #:for #,(scope-ref #'scope))]
    [(_ name:expr body #:for scope)
     #`(f:run name (forge-body body) #:for #,(scope-ref #'scope))]
    [(_ name:expr #:for scope body)
     #`(f:run name (forge-body body) #:for #,(scope-ref #'scope))]))

(define-syntax (forge-check stx)
  (syntax-parse stx
    [(_ name:id ((~datum block) premise:id conclusion:id))
     #'(f:check 'name (f:=> 'premise 'conclusion))]
    [(_ name:id body option ...)
     #'(f:check 'name (forge-body body) option ...)]
    [(_ name:expr body option ...)
     #'(f:check name (forge-body body) option ...)]))

(begin-for-syntax
  (define (syntax-list ctx pieces)
    (datum->syntax ctx pieces ctx ctx))

  (define (quantifier-binding-set pieces-stx)
    (define pieces (syntax->list pieces-stx))
    (cond
      [(= (length pieces) 1)
       (car pieces)]
      [(and (= (length pieces) 3)
            (eq? (syntax-e (cadr pieces)) '|.|))
       #`(#,(car pieces) #,(caddr pieces))]
      [else
       #`(#%rewrite-infix (#,@pieces))]))

  (define-syntax-class quantifier-binding
    #:attributes (var set)
    [pattern ((~datum #%seq) (var:id set-piece ...+))
     #:attr set
     (quantifier-binding-set #'(set-piece ...))])

  (define (block-form? stx)
    (syntax-parse stx
      [((~datum block) _ ...) #t]
      [_ #f]))

  (define (comma? stx)
    (eq? (syntax-e stx) '|,|))

  (define (split-on-commas pieces)
    (let loop ([groups '()]
               [group '()]
               [remaining pieces])
      (cond
        [(null? remaining)
         (reverse (cons (reverse group) groups))]
        [(comma? (car remaining))
         (loop (cons (reverse group) groups) '() (cdr remaining))]
        [else
         (loop groups (cons (car remaining) group) (cdr remaining))])))

  (define (parse-quantifier-binding group parse ctx)
    (syntax-parse (syntax-list ctx group)
      [(var:id (~datum in) set-piece ...+)
       (list #'var (parse #'(set-piece ...)))]
      [_
       (raise-syntax-error #f
                           "expected a quantifier binding like `x ∈ set`"
                           (syntax-list ctx group))]))

  (define (parse-quantifier kind stx parse)
    (define tokens (cdr (syntax->list stx)))
    (when (or (null? tokens)
              (not (block-form? (last tokens))))
      (raise-syntax-error #f
                          "expected comma-separated bindings followed by an indented body"
                          stx))
    (define body (last tokens))
    (define binding-tokens (drop-right tokens 1))
    (define binding-groups (split-on-commas binding-tokens))
    (define bindings
      (for/list ([group (in-list binding-groups)])
        (parse-quantifier-binding group parse stx)))
    (with-syntax ([(var ...) (map first bindings)]
                  [(set ...) (map second bindings)]
                  [body body])
      (case kind
        [(all)
         #'(f:all ([var set] ...) (forge-body body))]
        [(some)
         #'(f:some ([var set] ...) (forge-body body))]
        [else
         (raise-syntax-error #f "unknown quantifier kind" stx)]))))

(define-syntax (forge-forall stx parse)
  (parse-quantifier 'all stx parse))

(define-syntax (forge-all stx)
  (syntax-parse stx
    [(_ (binding:quantifier-binding ...) body)
     #'(f:all ([binding.var binding.set] ...)
              (forge-body body))]
    [(_ bindings body)
     #'(f:all bindings (forge-body body))]))

(define-syntax (forge-some stx)
  (syntax-parse stx
    [(_ (binding:quantifier-binding ...) body)
     #'(f:some ([binding.var binding.set] ...)
               (forge-body body))]
    [(_ bindings body)
     #'(f:some bindings (forge-body body))]
    [(_ body)
     #'(f:some body)]))

(define-syntax (forge-exists stx parse)
  (parse-quantifier 'some stx parse))

(define-syntax (forge-lone stx)
  (syntax-parse stx
    [(_ (binding:quantifier-binding ...) body)
     #'(f:lone ([binding.var binding.set] ...)
               (forge-body body))]
    [(_ bindings body)
     #'(f:lone bindings (forge-body body))]
    [(_ body)
     #'(f:lone body)]))

(define-syntax (forge-always stx)
  (syntax-parse stx
    [(_ body)
     #'(f:always (forge-body body))]))

(define-syntax (forge-next-state stx)
  (syntax-parse stx
    [(_ body)
     #'(f:next-state (forge-body body))]))
