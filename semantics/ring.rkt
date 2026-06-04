#lang racket
(require redex/reduction-semantics)

(define-language Ring
  (r ::= (ring n n n))
  (n ::= natural))

(define-metafunction Ring
  fill : r -> n
  [(fill (ring n_a n_b n_c))
   ,(- (term n_b) (term n_c))])

(define-metafunction Ring
  free : r -> n
  [(free (ring n_a n_b n_c))
   ,(- (term n_a) (term (fill (ring n_a n_b n_c))))])

(define ring->
  (reduction-relation
   Ring
   #:domain r

   (--> (ring n_a n_b n_c)
        (ring n_a ,(+ 1 (term n_b)) n_c)
        (side-condition
         (positive? (term (free (ring n_a n_b n_c)))))
        produce)

   (--> (ring n_a n_b n_c)
        (ring n_a n_b ,(+ 1 (term n_c)))
        (side-condition
         (positive? (term (fill (ring n_a n_b n_c)))))
        consume)))

(define-metafunction Ring
  valid? : r -> boolean
  [(valid? (ring n_a n_b n_c))
   ,(and (<= (term n_c) (term n_b))
         (<= (term (fill (ring n_a n_b n_c)))
             (term n_a)))])

(define (step-preserves-validity? r)
  (or (not (term (valid? ,r)))
      (for/and ([r* (in-list (apply-reduction-relation ring-> r))])
        (term (valid? ,r*)))))

(define (take-named-step r move)
  (define nexts (apply-reduction-relation/tag-with-names ring-> r))
  (define move-name (symbol->string move))
  (define selected
    (for/first ([step (in-list nexts)]
                #:when (equal? (first step) move-name))
      (second step)))
  (unless selected
    (error 'take-named-step
           "move ~a is not enabled from ~a; enabled moves: ~a"
           move
           r
           (map first nexts)))
  selected)

(module+ main
  (printf "~n=== a ring of capacity 2, starting empty ===~n")
  (printf "start: ~a   fill=~a free=~a~n"
          (term (ring 2 0 0))
          (term (fill (ring 2 0 0)))
          (term (free (ring 2 0 0))))

  ;; What can the empty ring do? (only produce)
  (printf "~nfrom (ring 2 0 0), one step can reach:~n  ~a~n"
          (apply-reduction-relation ring-> (term (ring 2 0 0))))

  ;; What can a full ring do? (only consume)
  (printf "~nfrom (ring 2 2 0)  [full], one step can reach:~n  ~a~n"
          (apply-reduction-relation ring-> (term (ring 2 2 0))))

  ;; What can a half-full ring do? (both — produce OR consume)
  (printf "~nfrom (ring 2 1 0)  [half], one step can reach:~n  ~a~n"
          (apply-reduction-relation ring-> (term (ring 2 1 0))))

  ;; A full run: produce twice, consume twice, watch the cursors climb.
  (printf "~n=== a hand trace: fill it, drain it ===~n")
  (void
   (for/fold ([r (term (ring 2 0 0))])
             ([move (in-list '(produce produce consume consume))])
     (define r* (take-named-step r move))
     (printf "~a  --~a-->  ~a~n" r move r*)
     r*))

  ;; The invariant: try hard to break it with random valid states.
  (printf "~n=== redex-check: can any single step break 0 <= fill <= cap? ===~n")
  (redex-check Ring r (step-preserves-validity? (term r))
               #:attempts 5000))
