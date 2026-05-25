#lang racket/base

(require (for-syntax racket/base
                     syntax/parse)
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
                    block)
         (prefix-in f: "model.rkt"))

(provide (all-from-out "ontology.rkt")
         ontology
         class
         property
         option
         field
         one
         no
         =>
         &&
         ||
         either
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
                     [forge-some some]
                     [forge-lone lone]
                     [forge-always always]
                     [forge-next-state next-state]))

(define either f:||)

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
    [((~datum class) name:id option ...)
     #'name]
    [((~datum property) name:id option ...)
     #'name]))

(define-syntax (ontology stx)
  (syntax-parse stx
    [(_ name:id base:expr ((~datum block) clause ...))
     (define clause-bindings
       (map ontology-clause-binding (syntax->list #'(clause ...))))
     #`(begin
         (o:define-ontology name base)
         (ontology-clause name clause) ...
         (provide name #,@clause-bindings))]))

(define-syntax (ontology-clause stx)
  (syntax-parse stx
    [(_ ont:id ((~datum class) name:id))
     #'(o:define-class ont name)]
    [(_ ont:id ((~datum class) name:id option ...))
     #'(o:define-class ont name option ...)]
    [(_ ont:id ((~datum property) name:id ((~datum block) (domain:id range:id) ...)))
     #'(o:define-property ont name ((domain range) ...))]
    [(_ ont:id ((~datum property) name:id ((~datum block))))
     #'(o:define-property ont name)]
    [(_ ont:id ((~datum property) name:id))
     #'(o:define-property ont name)]
    [(_ ont:id ((~datum property) name:id (domain:id range:id) ...))
     #'(o:define-property ont name ((domain range) ...))]))

(define-syntax (class stx)
  (raise-syntax-error 'class "class is only valid inside an ontology block" stx))

(define-syntax (property stx)
  (raise-syntax-error 'property "property is only valid inside an ontology block" stx))

(define-syntax (option stx)
  (syntax-parse stx
    [(_ name:id value:expr)
     #'(f:option 'name value)]
    [(_ name:expr value:expr)
     #'(f:option name value)]))

(define-syntax (forge-body stx)
  (syntax-parse stx
    [(_ ((~datum block) body:expr ...))
     #'(f:block body ...)]
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

(define-syntax (forge-run stx)
  (syntax-parse stx
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

(define-syntax (forge-all stx)
  (syntax-parse stx
    [(_ (((~datum #%seq) (var:id set:expr)) ...) body)
     #'(f:all ([var set] ...) (forge-body body))]
    [(_ bindings body)
     #'(f:all bindings (forge-body body))]))

(define-syntax (forge-some stx)
  (syntax-parse stx
    [(_ (((~datum #%seq) (var:id set:expr)) ...) body)
     #'(f:some ([var set] ...) (forge-body body))]
    [(_ bindings body)
     #'(f:some bindings (forge-body body))]
    [(_ body)
     #'(f:some body)]))

(define-syntax (forge-lone stx)
  (syntax-parse stx
    [(_ (((~datum #%seq) (var:id set:expr)) ...) body)
     #'(f:lone ([var set] ...) (forge-body body))]
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
