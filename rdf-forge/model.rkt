#lang racket/base

(require racket/list
         racket/match
         racket/file
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
 count
 ge
 always
 model->forge-runs
 run-forge-model
 check-forge-model
 write-forge-run-xml
 )

(struct compiled-forge-model (sigs relations predicates runs checks options) #:transparent)

(struct forge-model (language options signatures predicates checks runs) #:transparent)
(struct forge-signature (term fields) #:transparent)
(struct forge-field (term multiplicity range) #:transparent)
(struct forge-predicate (name body) #:transparent)
(struct forge-run (name body scope) #:transparent)
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

(define (count expr)
  (forge-expr 'count (list expr)))

(define (ge left right)
  (forge-expr 'ge (list left right)))

(define (always body)
  (forge-expr 'always (list body)))

(define-syntax (all stx)
  (syntax-parse stx
    [(_ ([var:id set:expr] ...) body:expr)
     #'(let ([var 'var] ...)
         (forge-quant 'all
                      (list (cons 'var set) ...)
                      body))]))

(define-syntax (some stx)
  (syntax-parse stx
    [(_ ([var:id set:expr] ...) body:expr)
     #'(let ([var 'var] ...)
         (forge-quant 'some
                      (list (cons 'var set) ...)
                      body))]
    [(_ expr:expr)
     #'(forge-expr 'some (list expr))]))

(define-syntax (lone stx)
  (syntax-parse stx
    [(_ ([var:id set:expr] ...) body:expr)
     #'(let ([var 'var] ...)
         (forge-quant 'lone
                      (list (cons 'var set) ...)
                      body))]
    [(_ expr:expr)
     #'(forge-expr 'lone (list expr))]))

(define (make-field term multiplicity range)
  (forge-field term multiplicity range))

(define-syntax (field stx)
  (syntax-parse stx
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
    [(_ other:expr)
     #'other]))

(define (predicate name body)
  (forge-predicate name body))

(define (check name body #:for [scope 'default] #:expect [expect 'checked])
  (forge-check name body scope expect))

(define (run name body #:for scope)
  (forge-run name body scope))

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
               (reverse signatures)
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
     (ontology-property (term-ontology domain-term)
                        (forge-field-ref-name term)
                        #:domain domain-term
                        #:range (forge-field-range fld))]
    [else term]))

(define (forge-option-hash model #:run-sterling [run-sterling #f] #:export-run [export-run #f] #:export-xml [export-xml #f])
  (define base
    (for/fold ([options (hash)])
              ([opt (in-list (forge-model-options model))])
      (define name (forge-option-name opt))
      (if (eq? name 'verbose)
          (hash-set (hash-set options 'verbosity (forge-option-value opt))
                    'engine_verbosity
                    (forge-option-value opt))
          (hash-set options name (forge-option-value opt)))))
  (define with-defaults
    (for/fold ([options f:DEFAULT-OPTIONS])
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
                                (list domain range))))
  relation-map)

(define (compile-field-constraints signatures sig-map relation-map)
  (for*/list ([sig (in-list signatures)]
              [fld (in-list (forge-signature-fields sig))]
              #:unless (eq? (forge-field-multiplicity fld) 'set))
    (define domain (hash-ref sig-map (forge-signature-term sig)))
    (define relation-term (field-term-for-domain (forge-signature-term sig) fld))
    (define relation (hash-ref relation-map relation-term))
    (define var (f:var (string->symbol (string-append (forge-name (forge-signature-term sig)) "_self"))))
    (define relation-value (f:join/func var relation))
    (define body
      (case (forge-field-multiplicity fld)
        [(one) (f:one/func relation-value)]
        [(lone) (f:lone/func relation-value)]
        [else
         (raise-argument-error 'compile-field-constraints
                               "field multiplicity"
                               (forge-field-multiplicity fld))]))
    (f:all-quant/func (list (cons var domain)) body)))

(define (compile-symbol sym env predicate-map)
  (cond
    [(hash-has-key? env sym) (hash-ref env sym)]
    [(hash-has-key? predicate-map sym) (hash-ref predicate-map sym)]
    [else (raise-argument-error 'compile-symbol "bound Forge variable or predicate name" sym)]))

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
       [(hash-has-key? relation-map expr) (hash-ref relation-map expr)]
       [else (raise-argument-error 'compile-forge-expr "model term" expr)])]
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
       ['count (f:card/func (compile-arg (first args)))]
       ['ge (f:||/func
             (f:int>/func (compile-arg (first args)) (compile-arg (second args)))
             (f:int=/func (compile-arg (first args)) (compile-arg (second args))))]
       ['always (f:always/func (compile-arg (first args)))]
       [other (raise-argument-error 'compile-forge-expr "known forge expression" other)])]
    [else (raise-argument-error 'compile-forge-expr "forge expression" expr)]))

(define (scope->forge scope sigs)
  (cond
    [(eq? scope 'default) '()]
    [(integer? scope)
     (for/list ([sig (in-list sigs)])
       (list sig scope))]
    [else scope]))

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
  (define (run-with-field-constraints body)
    (apply f:&&/func (append field-constraints (list body))))
  (define (check-with-field-constraints body)
    (if (null? field-constraints)
        body
        (f:=>/func (apply f:&&/func field-constraints) body)))
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
      (values name
              (f:make-run #:name name
                          #:preds (list run-body)
                          #:scope (scope->forge (forge-run-scope command) sigs)
                          #:sigs sigs
                          #:relations relations
                          #:options options))))
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

(define (write-forge-run-xml run output-path)
  (define inst (f:tree:get-value (f:Run-result run)))
  (set-box! (f:Run-last-sterling-instance run) inst)
  (define run-spec (f:Run-run-spec run))
  (define options (f:State-options (f:Run-spec-state run-spec)))
  (define xml
    (solution-to-XML-string inst
                            (f:get-relation-map run)
                            (f:Run-name run)
                            (format "(run ~a)" (f:Run-name run))
                            "/dev/null"
                            (f:get-bitwidth run-spec)
                            forge-version
                            #:tuple-annotations (hash)
                            #:run-options options))
  (make-parent-directory* output-path)
  (call-with-output-file output-path
    (lambda (out) (display xml out))
    #:exists 'replace))
