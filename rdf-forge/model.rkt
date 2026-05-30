#lang racket/base

(require racket/list
         racket/match
         racket/file
         racket/port
         racket/string
         xml
         "ontology.rkt"
         (only-in forge/choose-lang-specific set-checker-hash! set-ast-checker-hash!)
         (only-in forge/lang/lang-specific-checks forge-checker-hash forge-ast-checker-hash)
         (only-in forge/temporal/lang/temporal-lang-specific-checks temporal-checker-hash temporal-ast-checker-hash)
         (only-in forge/server/modelToXML solution-to-XML-string)
         (only-in forge/shared forge-version)
         (prefix-in f: forge/sigs-functional)
         (for-syntax racket/base
                     syntax/parse))

(provide
 (struct-out forge-model)
 (struct-out forge-signature)
 (struct-out forge-field)
 (struct-out forge-predicate)
 (struct-out forge-run)
 (struct-out forge-check)
 (struct-out forge-option)
 (struct-out forge-expr)
 (struct-out forge-quant)
 (struct-out forge-field-ref)
 model
 signature
 field
 predicate
 run
 check
 option
 block
 all
 some
 lone
 one
 no
 =>
 &&
 ||
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
 always
 next-state
 prime
 model->forge-runs
 run-forge-model
 check-forge-model
 write-forge-run-xml
 forge-run->text
 )

(struct compiled-forge-model (sigs relations predicates runs checks options) #:transparent)

(struct forge-model (language options signatures predicates checks runs) #:transparent)
(struct forge-signature (term fields) #:transparent)
(struct forge-field (term multiplicity range variable?) #:transparent)
(struct forge-predicate (name body) #:transparent)
(struct forge-run (name body scope options) #:transparent)
(struct forge-check (name body scope expect) #:transparent)
(struct forge-option (name value) #:transparent)
(struct forge-expr (op args) #:transparent)
(struct forge-quant (kind bindings body) #:transparent)
(struct forge-field-ref (name) #:transparent)

(define (option name value)
  (forge-option name value))

(define (block . body)
  (forge-expr 'block body))

(define (one expr)
  (forge-expr 'one (list expr)))

(define (no expr)
  (forge-expr 'no (list expr)))

(define (=> left right)
  (forge-expr '=> (list left right)))

(define (&& . body)
  (forge-expr '&& body))

(define (|| . body)
  (forge-expr '|| body))

(define (== left right)
  (forge-expr '== (list left right)))

(define (in left right)
  (forge-expr 'in (list left right)))

(define (join left right)
  (forge-expr 'join (list left right)))

(define (rjoin left right)
  (forge-expr 'rjoin (list left right)))

(define (follow first-step . next-steps)
  (for/fold ([expr first-step])
            ([step (in-list next-steps)])
    (join expr step)))

(define (matching relation value)
  (rjoin relation value))

(define (union . body)
  (forge-expr 'union body))

(define (intersect . body)
  (forge-expr 'intersect body))

(define (count expr)
  (forge-expr 'count (list expr)))

(define (ge left right)
  (forge-expr 'ge (list left right)))

(define (always body)
  (forge-expr 'always (list body)))

(define (next-state body)
  (forge-expr 'next-state (list body)))

(define (prime expr)
  (forge-expr 'prime (list expr)))

(define-syntax (with-forge-vars stx)
  (syntax-parse stx
    [(_ (var:id ...) body:expr)
     #'(let-syntax ([var (lambda (use-stx)
                           (syntax-parse use-stx
                             [id:id #''var]
                             [(_ step (... ...))
                              #'(follow 'var step (... ...))]))]
                    ...)
         body)]))

(define-syntax (all stx)
  (syntax-parse stx
    [(_ ([var:id set] ...) body)
     #'(with-forge-vars (var ...)
         (forge-quant 'all
                      (list (cons 'var set) ...)
                      body))]))

(define-syntax (some stx)
  (syntax-parse stx
    [(_ ([var:id set] ...) body)
     #'(with-forge-vars (var ...)
         (forge-quant 'some
                      (list (cons 'var set) ...)
                      body))]
    [(_ expr)
     #'(forge-expr 'some (list expr))]))

(define-syntax (lone stx)
  (syntax-parse stx
    [(_ ([var:id set] ...) body)
     #'(with-forge-vars (var ...)
         (forge-quant 'lone
                      (list (cons 'var set) ...)
                      body))]
    [(_ expr)
     #'(forge-expr 'lone (list expr))]))

(define (make-field term multiplicity range #:variable? [variable? #f])
  (forge-field term multiplicity range variable?))

(define-syntax (field stx)
  (syntax-parse stx
    [(_ name:id #:var #:one range:expr)
     #'(make-field (forge-field-ref 'name) 'one range #:variable? #t)]
    [(_ name:id #:var #:lone range:expr)
     #'(make-field (forge-field-ref 'name) 'lone range #:variable? #t)]
    [(_ name:id #:var #:set range:expr)
     #'(make-field (forge-field-ref 'name) 'set range #:variable? #t)]
    [(_ name:id #:var range:expr)
     #'(make-field (forge-field-ref 'name) 'one range #:variable? #t)]
    [(_ name:id #:one range:expr)
     #'(make-field (forge-field-ref 'name) 'one range)]
    [(_ name:id #:lone range:expr)
     #'(make-field (forge-field-ref 'name) 'lone range)]
    [(_ name:id #:set range:expr)
     #'(make-field (forge-field-ref 'name) 'set range)]
    [(_ name:id range:expr)
     #'(make-field (forge-field-ref 'name) 'one range)]
    [(_ term:expr #:one range:expr)
     #'(make-field term 'one range)]
    [(_ term:expr #:lone range:expr)
     #'(make-field term 'lone range)]
    [(_ term:expr #:set range:expr)
     #'(make-field term 'set range)]
    [(_ term:expr #:var #:one range:expr)
     #'(make-field term 'one range #:variable? #t)]
    [(_ term:expr #:var #:lone range:expr)
     #'(make-field term 'lone range #:variable? #t)]
    [(_ term:expr #:var #:set range:expr)
     #'(make-field term 'set range #:variable? #t)]
    [(_ term:expr #:var range:expr)
     #'(make-field term 'one range #:variable? #t)]
    [(_ term:expr range:expr)
     #'(make-field term 'one range)]))

(define (make-signature term . fields)
  (forge-signature term fields))

(define-syntax (signature stx)
  (syntax-parse stx
    [(_ term:expr)
     #'(make-signature term)]
    [(_ term:expr field-clause ...)
     #'(make-signature term (signature-field field-clause) ...)]))

(define-syntax (signature-field stx)
  (syntax-parse stx
    [(_ ((~datum field) arg ...))
     #'(field arg ...)]
    [(_ (name:id (~datum one) range:expr))
     #'(make-field (forge-field-ref 'name) 'one range)]
    [(_ (name:id (~datum lone) range:expr))
     #'(make-field (forge-field-ref 'name) 'lone range)]
    [(_ (name:id (~datum set) range:expr))
     #'(make-field (forge-field-ref 'name) 'set range)]
    [(_ (name:id (~datum var) (~datum one) range:expr))
     #'(make-field (forge-field-ref 'name) 'one range #:variable? #t)]
    [(_ (name:id (~datum var) (~datum lone) range:expr))
     #'(make-field (forge-field-ref 'name) 'lone range #:variable? #t)]
    [(_ (name:id (~datum var) (~datum set) range:expr))
     #'(make-field (forge-field-ref 'name) 'set range #:variable? #t)]
    [(_ other:expr)
     #'other]))

(define (predicate name body)
  (forge-predicate name body))

(define (check name body #:for [scope 'default] #:expect [expect 'checked])
  (forge-check name body scope expect))

(define (run name body
             #:for scope
             #:min-tracelength [min-tracelength #f]
             #:max-tracelength [max-tracelength #f])
  (forge-run name body scope
             (append (if min-tracelength
                         (list (forge-option 'min_tracelength min-tracelength))
                         '())
                     (if max-tracelength
                         (list (forge-option 'max_tracelength max-tracelength))
                         '()))))

(define (ontology-class-terms ont)
  (filter (lambda (value)
            (and (term? value)
                 (eq? (term-kind value) 'class)))
          (ontology-declared-terms ont)))

(define (ontology-field-terms ont)
  (filter (lambda (value)
            (and (term? value)
                 (eq? (term-kind value) 'property)
                 (term-option value 'domain)
                 (term-option value 'range)))
          (ontology-declared-terms ont)))

(define (ontology->signatures ont)
  (define class-terms (ontology-class-terms ont))
  (define fields-by-domain
    (for/fold ([fields (hasheq)])
              ([property (in-list (ontology-field-terms ont))])
      (define domain (term-option property 'domain))
      (define range (term-option property 'range))
      (define fld
        (make-field property
                    (term-option property 'multiplicity 'set)
                    range
                    #:variable? (and (term-option property 'variable?) #t)))
      (hash-update fields domain (lambda (existing) (append existing (list fld))) '())))
  (for/list ([class-term (in-list class-terms)])
    (apply make-signature
           class-term
           (hash-ref fields-by-domain class-term '()))))

(define (merge-signature-list signatures)
  (define-values (order fields-by-term)
    (for/fold ([order '()]
               [fields (hasheq)])
              ([sig (in-list signatures)])
      (define term (forge-signature-term sig))
      (define first? (not (hash-has-key? fields term)))
      (values (if first? (append order (list term)) order)
              (hash-update fields
                           term
                           (lambda (existing)
                             (append existing (forge-signature-fields sig)))
                           '()))))
  (for/list ([term (in-list order)])
    (apply make-signature term (hash-ref fields-by-term term))))

(define (model #:language [language 'forge/temporal] . parts)
  (define-values (options signatures predicates checks runs)
    (for/fold ([options '()]
               [signatures '()]
               [predicates '()]
               [checks '()]
               [runs '()])
              ([part (in-list parts)])
      (cond
        [(forge-option? part)
         (values (cons part options) signatures predicates checks runs)]
        [(forge-signature? part)
         (values options (cons part signatures) predicates checks runs)]
        [(ontology? part)
         (values options
                 (append (reverse (ontology->signatures part)) signatures)
                 predicates
                 checks
                 runs)]
        [(forge-predicate? part)
         (values options signatures (cons part predicates) checks runs)]
        [(forge-check? part)
         (values options signatures predicates (cons part checks) runs)]
        [(forge-run? part)
         (values options signatures predicates checks (cons part runs))]
        [else
         (raise-argument-error 'model "model part" part)])))
  (forge-model language
               (reverse options)
               (merge-signature-list (reverse signatures))
               (reverse predicates)
               (reverse checks)
               (reverse runs)))

(define (forge-name value)
  (cond
    [(term? value) (term-forge-name value)]
    [(symbol? value) (symbol->string value)]
    [else (format "~a" value)]))

(define (term-option value key [fallback #f])
  (define found (and (term? value) (assoc key (term-options value))))
  (if found (cdr found) fallback))

(define (field-term-for-domain domain-term fld)
  (define term (forge-field-term fld))
  (cond
    [(forge-field-ref? term)
     (ontology-ensure-property (term-ontology domain-term)
                               (forge-field-ref-name term)
                               #:domain domain-term
                               #:range (forge-field-range fld))]
    [else term]))

(define (variant-children range-term signatures)
  (define children (term-option range-term 'variant-children))
  (and children
       (for/list ([child-local (in-list children)])
         (or (for/first ([sig (in-list signatures)]
                         #:when (eq? (term-local (forge-signature-term sig))
                                     child-local))
               (forge-signature-term sig))
             (raise-argument-error 'variant-children
                                   "variant child signature"
                                   child-local)))))

(define (forge-option-hash model
                           #:run-options [run-options '()]
                           #:run-sterling [run-sterling #f]
                           #:export-run [export-run #f]
                           #:export-xml [export-xml #f])
  (define base
    (for/fold ([options (hash)])
              ([opt (in-list (append (forge-model-options model)
                                     run-options))])
      (define name (forge-option-name opt))
      (if (eq? name 'verbose)
          (hash-set (hash-set options 'verbosity (forge-option-value opt))
                    'engine_verbosity
                    (forge-option-value opt))
          (hash-set options name (forge-option-value opt)))))
  (define with-defaults
    (for/fold ([options (hash-set (hash-set f:DEFAULT-OPTIONS
                                            'verbosity 0)
                                  'engine_verbosity 0)])
              ([(key value) (in-hash base)])
      (hash-set options key value)))
  (define with-language
    (if (eq? (forge-model-language model) 'forge/temporal)
        (hash-set with-defaults 'problem_type 'temporal)
        with-defaults))
  (define with-sterling
    (if run-sterling (hash-set with-language 'run_sterling run-sterling) with-language))
  (define with-export-run
    (if export-run (hash-set with-sterling 'export_run export-run) with-sterling))
  (if export-xml (hash-set with-export-run 'export_xml export-xml) with-export-run))

(define (signature-parent-term sig signature-terms)
  (define parent (term-option (forge-signature-term sig) 'subclass-of))
  (and parent (member parent signature-terms) parent))

(define (compile-signatures signatures)
  (define signature-terms (map forge-signature-term signatures))
  (define sig-map (make-hasheq))
  (for ([sig (in-list signatures)])
    (define term (forge-signature-term sig))
    (hash-set! sig-map term
               (f:make-sig (string->symbol (forge-name term))
                           #:abstract (and (term-option term 'abstract) #t))))
  (for ([sig (in-list signatures)])
    (define parent-term (signature-parent-term sig signature-terms))
    (when parent-term
      (define term (forge-signature-term sig))
      (hash-set! sig-map term
                 (f:make-sig (string->symbol (forge-name term))
                             #:abstract (and (term-option term 'abstract) #t)
                             #:extends (hash-ref sig-map parent-term)))))
  sig-map)

(define (compile-relations signatures sig-map)
  (define relation-map (make-hasheq))
  (for* ([sig (in-list signatures)]
         [fld (in-list (forge-signature-fields sig))])
    (define domain-term (forge-signature-term sig))
    (define domain (hash-ref sig-map domain-term))
    (define range (hash-ref sig-map (forge-field-range fld)))
    (define relation-term (field-term-for-domain domain-term fld))
    (hash-set! relation-map
               relation-term
               (f:make-relation (string->symbol (forge-name relation-term))
                                (list domain range)
                                #:is-var (forge-field-variable? fld))))
  relation-map)

(define (compile-field-constraints signatures sig-map relation-map)
  (apply
   append
   (for*/list ([sig (in-list signatures)]
               [fld (in-list (forge-signature-fields sig))])
    (define domain (hash-ref sig-map (forge-signature-term sig)))
    (define relation-term (field-term-for-domain (forge-signature-term sig) fld))
    (define relation (hash-ref relation-map relation-term))
    (define var (f:var (string->symbol (string-append (forge-name (forge-signature-term sig)) "_self"))))
    (define relation-value (f:join/func var relation))
    (define variant-cases
      (variant-children (forge-field-range fld) signatures))
    (define multiplicity-body
      (case (forge-field-multiplicity fld)
        [(set) #f]
        [(one) (f:one/func relation-value)]
        [(lone) (f:lone/func relation-value)]
        [else
         (raise-argument-error 'compile-field-constraints
                               "field multiplicity"
                               (forge-field-multiplicity fld))]))
    (define variant-body
      (and variant-cases
           (f:in/func
            relation-value
            (apply f:+/func
                   (map (lambda (child) (hash-ref sig-map child))
                        variant-cases)))))
    (for/list ([body (in-list (filter values
                                      (list multiplicity-body
                                            variant-body)))])
      (f:all-quant/func (list (cons var domain)) body)))))

(define (compile-symbol sym env predicate-map)
  (cond
    [(hash-has-key? env sym) (hash-ref env sym)]
    [(hash-has-key? predicate-map sym) (hash-ref predicate-map sym)]
    [else (raise-argument-error 'compile-symbol "bound Forge variable or predicate name" sym)]))

(define (resolve-relation-term expr relation-map)
  (cond
    [(hash-has-key? relation-map expr)
     (hash-ref relation-map expr)]
    [else
     (define local (term-rdf-name expr))
     (define matches
       (for/list ([(relation-term relation) (in-hash relation-map)]
                  #:when (eq? (term-rdf-name relation-term) local))
         relation))
     (case (length matches)
       [(1) (car matches)]
       [(0) #f]
       [else (raise-argument-error 'compile-forge-expr
                                   "unambiguous model relation"
                                   expr)])]))

(define (compile-quantifier quant sig-map relation-map predicate-map env)
  (define-values (decls body-env)
    (for/fold ([decls '()]
               [next-env env])
              ([binding (in-list (forge-quant-bindings quant))])
      (define var-name (car binding))
      (define domain (compile-forge-expr (cdr binding) sig-map relation-map predicate-map next-env))
      (define qvar (f:var var-name))
      (values (append decls (list (cons qvar domain)))
              (hash-set next-env var-name qvar))))
  (define body (compile-forge-expr (forge-quant-body quant) sig-map relation-map predicate-map body-env))
  (case (forge-quant-kind quant)
    [(all) (f:all-quant/func decls body)]
    [(some) (f:some-quant/func decls body)]
    [(no) (f:no-quant/func decls body)]
    [(one) (f:one-quant/func decls body)]
    [(lone) (f:lone-quant/func decls body)]
    [else (raise-argument-error 'compile-quantifier "known quantifier" (forge-quant-kind quant))]))

(define (compile-forge-expr expr sig-map relation-map predicate-map [env (hash)])
  (cond
    [(number? expr) (f:int/func expr)]
    [(term? expr)
     (cond
       [(hash-has-key? sig-map expr) (hash-ref sig-map expr)]
       [else
        (define relation (resolve-relation-term expr relation-map))
        (if relation
            relation
            (raise-argument-error 'compile-forge-expr "model term" expr))])]
    [(symbol? expr) (compile-symbol expr env predicate-map)]
    [(forge-quant? expr) (compile-quantifier expr sig-map relation-map predicate-map env)]
    [(forge-expr? expr)
     (define args (forge-expr-args expr))
     (define (compile-arg arg) (compile-forge-expr arg sig-map relation-map predicate-map env))
     (match (forge-expr-op expr)
       ['block (apply f:&&/func (map compile-arg args))]
       ['one (f:one/func (compile-arg (first args)))]
       ['no (f:no/func (compile-arg (first args)))]
       ['some (f:some/func (compile-arg (first args)))]
       ['lone (f:lone/func (compile-arg (first args)))]
       ['=> (f:=>/func (compile-arg (first args)) (compile-arg (second args)))]
       ['&& (apply f:&&/func (map compile-arg args))]
       ['|| (apply f:||/func (map compile-arg args))]
       ['== (f:=/func (compile-arg (first args)) (compile-arg (second args)))]
       ['in (f:in/func (compile-arg (first args)) (compile-arg (second args)))]
       ['join (f:join/func (compile-arg (first args)) (compile-arg (second args)))]
       ['rjoin (f:join/func (compile-arg (first args)) (compile-arg (second args)))]
       ['union (apply f:+/func (map compile-arg args))]
       ['intersect (apply f:&/func (map compile-arg args))]
       ['count (f:card/func (compile-arg (first args)))]
       ['ge (f:||/func
             (f:int>/func (compile-arg (first args)) (compile-arg (second args)))
             (f:int=/func (compile-arg (first args)) (compile-arg (second args))))]
       ['always (f:always/func (compile-arg (first args)))]
       ['next-state (f:next_state/func (compile-arg (first args)))]
       ['prime (f:prime/func (compile-arg (first args)))]
       [other (raise-argument-error 'compile-forge-expr "known forge expression" other)])]
    [else (raise-argument-error 'compile-forge-expr "forge expression" expr)]))

(define (scope->forge scope sigs)
  (cond
    [(eq? scope 'default) '()]
    [(integer? scope)
     (for/list ([sig (in-list sigs)])
       (list sig scope))]
    [(and (list? scope)
          (andmap (lambda (entry)
                    (and (list? entry)
                         (pair? entry)
                         (or (symbol? (car entry))
                             (string? (car entry)))))
                  scope))
     (define sig-by-name
       (for/hash ([sig (in-list sigs)])
         (values (f:Sig-name sig) sig)))
     (for/list ([entry (in-list scope)])
       (match entry
         [(list name upper)
          (list (hash-ref sig-by-name (scope-name->symbol name)) upper)]
         [(list name lower upper)
          (list (hash-ref sig-by-name (scope-name->symbol name)) lower upper)]
         [_ (raise-argument-error 'scope->forge
                                  "'(signature upper) or '(signature lower upper)"
                                  entry)]))]
    [else scope]))

(define (scope-name->symbol name)
  (if (symbol? name) name (string->symbol name)))

(define (model->compiled-forge model #:run-sterling [run-sterling #f] #:export-run [export-run #f] #:export-xml [export-xml #f])
  (cond
    [(eq? (forge-model-language model) 'forge/temporal)
     (set-checker-hash! temporal-checker-hash)
     (set-ast-checker-hash! temporal-ast-checker-hash)]
    [else
     (set-checker-hash! forge-checker-hash)
     (set-ast-checker-hash! forge-ast-checker-hash)])
  (define sig-map (compile-signatures (forge-model-signatures model)))
  (define relation-map (compile-relations (forge-model-signatures model) sig-map))
  (define field-constraints (compile-field-constraints (forge-model-signatures model) sig-map relation-map))
  (define predicate-map (make-hasheq))
  (for ([pred (in-list (forge-model-predicates model))])
    (hash-set! predicate-map
               (forge-predicate-name pred)
               (compile-forge-expr (forge-predicate-body pred) sig-map relation-map predicate-map)))
  (define sigs
    (for/list ([sig (in-list (forge-model-signatures model))])
      (hash-ref sig-map (forge-signature-term sig))))
  (define relations
    (for*/list ([sig (in-list (forge-model-signatures model))]
                [fld (in-list (forge-signature-fields sig))])
      (hash-ref relation-map (field-term-for-domain (forge-signature-term sig) fld))))
  (define options (forge-option-hash model #:run-sterling run-sterling #:export-run export-run #:export-xml export-xml))
  (define temporal? (eq? (forge-model-language model) 'forge/temporal))
  (define field-constraints-body
    (and (not (null? field-constraints))
         (apply f:&&/func field-constraints)))
  (define field-constraints-for-run
    (and field-constraints-body
         (if temporal?
             (f:always/func field-constraints-body)
             field-constraints-body)))
  (define (run-with-field-constraints body)
    (if field-constraints-for-run
        (f:&&/func field-constraints-for-run body)
        body))
  (define (check-with-field-constraints body)
    (cond
      [(not field-constraints-for-run) body]
      [else (f:=>/func field-constraints-for-run body)]))
  (define checks
    (for/list ([command (in-list (forge-model-checks model))])
      (define body (compile-forge-expr (forge-check-body command) sig-map relation-map predicate-map))
      (forge-check (forge-check-name command)
                   (check-with-field-constraints body)
                   (forge-check-scope command)
                   (forge-check-expect command))))
  (define runs
    (for/hash ([command (in-list (forge-model-runs model))])
      (define name (forge-run-name command))
      (define body (compile-forge-expr (forge-run-body command) sig-map relation-map predicate-map))
      (define run-body (run-with-field-constraints body))
      (define run-options
        (forge-option-hash model
                           #:run-options (forge-run-options command)
                           #:run-sterling run-sterling
                           #:export-run export-run
                           #:export-xml export-xml))
      (values name
              (f:make-run #:name name
                          #:preds (list run-body)
                          #:scope (scope->forge (forge-run-scope command) sigs)
                          #:sigs sigs
                          #:relations relations
                          #:options run-options))))
  (compiled-forge-model sigs relations predicate-map runs checks options))

(define (model->forge-runs model #:run-sterling [run-sterling #f] #:export-run [export-run #f] #:export-xml [export-xml #f])
  (compiled-forge-model-runs
   (model->compiled-forge model
                          #:run-sterling run-sterling
                          #:export-run export-run
                          #:export-xml export-xml)))

(define (run-forge-model model run-name #:run-sterling [run-sterling 'off] #:export-xml [export-xml #f])
  (define run
    (hash-ref (model->forge-runs model
                                 #:run-sterling run-sterling
                                 #:export-run run-name
                                 #:export-xml #f)
              run-name))
  (when export-xml
    (write-forge-run-xml run export-xml))
  run)

(define (check-forge-model model)
  (define compiled (model->compiled-forge model #:run-sterling 'off))
  (for/list ([command (in-list (compiled-forge-model-checks compiled))])
    (f:make-test #:name (forge-check-name command)
                 #:preds (list (forge-check-body command))
                 #:scope (scope->forge (forge-check-scope command)
                                       (compiled-forge-model-sigs compiled))
                 #:sigs (compiled-forge-model-sigs compiled)
                 #:relations (compiled-forge-model-relations compiled)
                 #:expect (forge-check-expect command)
                 #:options (compiled-forge-model-options compiled))))

(define (forge-run->xml-string run)
  (define inst (f:tree:get-value (f:Run-result run)))
  (set-box! (f:Run-last-sterling-instance run) inst)
  (define run-spec (f:Run-run-spec run))
  (define options (f:State-options (f:Run-spec-state run-spec)))
  (solution-to-XML-string inst
                          (f:get-relation-map run)
                          (f:Run-name run)
                          (format "(run ~a)" (f:Run-name run))
                          "/dev/null"
                          (f:get-bitwidth run-spec)
                          forge-version
                          #:tuple-annotations (hash)
                          #:run-options options))

(define (write-forge-run-xml run output-path)
  (define xml (forge-run->xml-string run))
  (make-parent-directory* output-path)
  (call-with-output-file output-path
    (lambda (out) (display xml out))
    #:exists 'replace))

(struct text-atom (id type) #:transparent)
(struct text-field (name tuples order) #:transparent)
(struct text-instance (command filename version atoms fields atom-order type-counts) #:transparent)

(define (forge-run->text run)
  (forge-xml->text (forge-run->xml-string run)))

(define (forge-xml->text xml)
  (define instances (parse-forge-xml xml))
  (cond
    [(null? instances)
     (error 'forge-xml->text "Forge XML did not contain an instance")]
    [(null? (cdr instances))
     (instance->text (car instances))]
    [else
     (trace->text instances)]))

(define (instance->text instance)
  (define lines
    (append
     (list (format "## ~a" (words (text-instance-command instance)))
           "")
     (instance-sentences instance)))
  (string-append (string-join lines "\n") "\n"))

(define (trace->text instances)
  (define heading (words (text-instance-command (car instances))))
  (define-values (lines previous-facts)
    (for/fold ([lines (list (format "## ~a" heading) "")]
               [previous-facts #f])
              ([instance (in-list instances)]
               [step (in-naturals 1)])
      (define facts (instance-atomic-facts instance))
      (define step-lines
        (cond
          [(not previous-facts)
           (append (list (format "### step ~a" step))
                   facts)]
          [else
           (define removed (set-subtract/string previous-facts facts))
           (define added (set-subtract/string facts previous-facts))
           (cond
             [(and (null? removed) (null? added))
              '()]
             [else
              (append
               (list (format "### step ~a" step))
               (for/list ([fact (in-list removed)])
                 (format "- ~a" fact))
               (for/list ([fact (in-list added)])
                 (format "+ ~a" fact)))])]))
      (values (append lines step-lines (list ""))
              facts)))
  (void previous-facts)
  (string-append (string-join (drop-right lines 1) "\n") "\n"))

(define (instance-sentences instance)
  (for/list ([atom-id (in-list (text-instance-atom-order instance))]
             #:do [(define phrases (atom-phrases instance atom-id))]
             #:when (pair? phrases))
    (format "~a ~a."
            (capitalize
             (pretty-atom atom-id
                          (text-atom-type
                           (hash-ref (text-instance-atoms instance) atom-id))
                          (text-instance-type-counts instance)))
            (join-english phrases))))

(define (instance-atomic-facts instance)
  (define atoms (text-instance-atoms instance))
  (define type-counts (text-instance-type-counts instance))
  (define order (for/hash ([id (in-list (text-instance-atom-order instance))]
                           [index (in-naturals)])
                  (values id index)))
  (define facts
    (for*/list ([field (in-list (text-instance-fields instance))]
                [tuple (in-list (sort-tuples (text-field-tuples field) order))]
                #:when (pair? tuple))
      (define relation (field-relation (text-field-name field)))
      (format "~a ~a ~a."
              (pretty-atom (car tuple)
                           (text-atom-type
                            (hash-ref atoms (car tuple) (text-atom (car tuple) "")))
                           type-counts)
              relation
              (pretty-tuple (cdr tuple) atoms type-counts relation))))
  (sort facts string<?))

(define (set-subtract/string left right)
  (filter (lambda (item) (not (member item right string=?))) left))

(define (parse-forge-xml xml)
  (define root
    (xml->xexpr
     (document-element
      (read-xml (open-input-string xml)))))
  (for/list ([instance (in-list (children-named root 'instance))])
    (parse-forge-instance instance)))

(define (parse-forge-instance instance)
  (define sig-names (make-hash))
  (define atoms (make-hash))
  (define type-counts (make-hash))
  (define atom-order '())
  (for ([sig (in-list (children-named instance 'sig))])
    (define id (attr-ref sig 'ID ""))
    (define label (attr-ref sig 'label id))
    (unless (or (string=? id "") (string=? (attr-ref sig 'builtin "") "yes"))
      (hash-set! sig-names id label)
      (for ([atom (in-list (children-named sig 'atom))])
        (define atom-id (attr-ref atom 'label ""))
        (unless (string=? atom-id "")
          (hash-set! atoms atom-id (text-atom atom-id label))
          (hash-update! type-counts label add1 0)
          (set! atom-order (append atom-order (list atom-id)))))))
  (define fields
    (for/list ([(field order) (in-indexed (children-named instance 'field))]
               #:do [(define name (attr-ref field 'label ""))]
               #:unless (or (string=? name "")
                            (string=? name "no-field-guard")))
      (text-field
       name
       (for/list ([tuple (in-list (children-named field 'tuple))])
         (for/list ([atom (in-list (children-named tuple 'atom))])
           (attr-ref atom 'label "")))
       order)))
  (text-instance
   (attr-ref instance 'command "")
   (attr-ref instance 'filename "")
   (attr-ref instance 'version "")
   atoms
   fields
   (sort atom-order atom-id<? #:key (lambda (atom-id)
                                      (hash-ref atoms atom-id)))
   type-counts))

(define (atom-id<? left right)
  (define left-type (words (text-atom-type left)))
  (define right-type (words (text-atom-type right)))
  (cond
    [(string<? left-type right-type) #t]
    [(string<? right-type left-type) #f]
    [else (string<? (text-atom-id left) (text-atom-id right))]))

(define (atom-phrases instance atom-id)
  (define atoms (text-instance-atoms instance))
  (define type-counts (text-instance-type-counts instance))
  (define order (for/hash ([id (in-list (text-instance-atom-order instance))]
                           [index (in-naturals)])
                  (values id index)))
  (for/list ([field (in-list (text-instance-fields instance))]
             #:do [(define matching
                     (filter (lambda (tuple)
                               (and (pair? tuple)
                                    (string=? (car tuple) atom-id)))
                             (text-field-tuples field)))]
             #:when (pair? matching))
    (define relation (field-relation (text-field-name field)))
    (define values
      (for/list ([tuple (in-list (sort-tuples (map cdr matching) order))])
        (pretty-tuple tuple atoms type-counts relation)))
    (format "~a ~a" relation (string-join values ", "))))

(define (sort-tuples tuples order)
  (sort tuples tuple<?
        #:cache-keys? #t
        #:key (lambda (tuple)
                (for/list ([atom-id (in-list tuple)])
                  (cons (hash-ref order atom-id +inf.0) atom-id)))))

(define (tuple<? left right)
  (cond
    [(and (null? left) (null? right)) #f]
    [(null? left) #t]
    [(null? right) #f]
    [(< (caar left) (caar right)) #t]
    [(> (caar left) (caar right)) #f]
    [(string<? (cdar left) (cdar right)) #t]
    [(string<? (cdar right) (cdar left)) #f]
    [else (tuple<? (cdr left) (cdr right))]))

(define (pretty-tuple tuple atoms type-counts relation)
  (cond
    [(null? tuple) "true"]
    [else
     (string-join
      (for/list ([atom-id (in-list tuple)]
                 [index (in-naturals)])
        (pretty-atom atom-id
                     (text-atom-type
                      (hash-ref atoms atom-id (text-atom atom-id "")))
                     type-counts
                     #:context (if (zero? index) relation "")))
      " to ")]))

(define (pretty-atom atom-id type type-counts #:context [context ""])
  (match-define (cons base number) (split-number-suffix atom-id))
  (define type-words (words (if (string=? type "") base type)))
  (define base-words (words base))
  (define (class-name value)
    (string-upcase value))
  (cond
    [(= (hash-ref type-counts type 0) 1)
     (class-name type-words)]
    [(and (not (string=? number ""))
          (context-names-type? context type-words))
     number]
    [(string=? number "") (class-name base-words)]
    [(same-words? type-words base-words)
     (format "~a ~a" (class-name type-words) (display-number number))]
    [else
     (format "~a ~a" (class-name base-words) (display-number number))]))

(define (display-number number)
  (number->string (add1 (string->number number))))

(define (field-relation name)
  (match (regexp-match #rx"^(.+)-for-.+$" name)
    [(list _ relation) (words relation)]
    [_ (words name)]))

(define (split-number-suffix value)
  (match (regexp-match #rx"^(.+?)([0-9]+)$" value)
    [(list _ base number) (cons base number)]
    [_ (cons value "")]))

(define (words value)
  (string-downcase
   (string-trim
    (regexp-replace*
     #rx"[[:space:]]+"
     (regexp-replace*
      #rx"[-_()]+"
      (regexp-replace*
       #rx"([a-z0-9])([A-Z])"
       (format "~a" value)
       "\\1 \\2")
      " ")
     " "))))

(define (same-words? left right)
  (string=? (regexp-replace* #rx"[[:space:]]+" left "")
            (regexp-replace* #rx"[[:space:]]+" right "")))

(define (context-names-type? context type)
  (and (not (string=? context ""))
       (not (string=? type ""))
       (let ([context-parts (string-split context)]
             [type-parts (string-split type)])
         (and (>= (length context-parts) (length type-parts))
              (equal? (take-right context-parts (length type-parts))
                      type-parts)))))

(define (join-english items)
  (match items
    ['() ""]
    [(list item) item]
    [(list left right) (format "~a and ~a" left right)]
    [_ (format "~a, and ~a"
               (string-join (drop-right items 1) ", ")
               (last items))]))

(define (capitalize value)
  (if (string=? value "")
      value
      (string-append (string-upcase (substring value 0 1))
                     (substring value 1))))

(define (find-child xexpr name)
  (for/or ([child (in-list (xexpr-children xexpr))])
    (and (pair? child) (eq? (car child) name) child)))

(define (children-named xexpr name)
  (for/list ([child (in-list (xexpr-children xexpr))]
             #:when (and (pair? child) (eq? (car child) name)))
    child))

(define (xexpr-attrs xexpr)
  (match xexpr
    [(list* (? symbol?) (and attrs (list (list (? symbol?) _) ...)) _) attrs]
    [_ '()]))

(define (xexpr-children xexpr)
  (match xexpr
    [(list* (? symbol?) (list (list (? symbol?) _) ...) children) children]
    [(list* (? symbol?) children) children]
    [_ '()]))

(define (attr-ref xexpr name [default #f])
  (match (assoc name (xexpr-attrs xexpr))
    [(list _ value) value]
    [_ default]))
