#lang racket
(require redex/reduction-semantics)

;; ============================================================
;; The pristine ring buffer, as a protocol.
;;
;; Strip away the bytes. Strip away the wraparound arithmetic.
;; What is left is the *protocol*: a bounded region with two
;; monotone cursors. `produced` only climbs; `consumed` only
;; climbs; and `consumed <= produced <= consumed + capacity`.
;;
;; The fill is the gap between the cursors (produced - consumed).
;; The free space is capacity minus fill.
;; You may PRODUCE only when there is free space (not full).
;; You may CONSUME only when there is fill (not empty).
;;
;; That is the whole ring. The monotonicity of the cursors IS
;; the irreversibility of time. The wraparound (cursor mod cap)
;; and the actual stored values are *realization* — they belong
;; to how you cash the protocol out onto physical land, not to
;; the protocol itself.
;; ============================================================

(define-language Ring
  ;; A ring is a capacity and two cursors. Nothing else.
  ;;   (ring  capacity  produced  consumed)
  (r ::= (ring n n n))
  (n ::= natural))

;; --- derived quantities, as metafunctions (just for reading) ---

(define-metafunction Ring
  fill : r -> n
  [(fill (ring n_cap n_p n_c)) ,(- (term n_p) (term n_c))])

(define-metafunction Ring
  free : r -> n
  [(free (ring n_cap n_p n_c)) ,(- (term n_cap) (term (fill (ring n_cap n_p n_c))))])

;; --- the protocol: exactly two rules ---

(define ring->
  (reduction-relation
   Ring
   #:domain r

   ;; PRODUCE: advance the produced cursor, if there is room.
   (--> (ring n_cap n_p n_c)
        (ring n_cap ,(add1 (term n_p)) n_c)
        (side-condition (positive? (term (free (ring n_cap n_p n_c)))))
        produce)

   ;; CONSUME: advance the consumed cursor, if there is stock.
   (--> (ring n_cap n_p n_c)
        (ring n_cap n_p ,(add1 (term n_c)))
        (side-condition (positive? (term (fill (ring n_cap n_p n_c)))))
        consume)))

;; ============================================================
;; The invariant the protocol is supposed to preserve:
;;   0 <= fill <= capacity     (equivalently 0 <= consumed <= produced
;;                              <= consumed + capacity)
;; A holder with a release policy is *correct* iff every step keeps
;; this true. We can ask Redex to try to break it.
;; ============================================================

(define-metafunction Ring
  valid? : r -> boolean
  [(valid? (ring n_cap n_p n_c))
   ,(and (<= (term n_c) (term n_p))                 ; consumed never passes produced
         (<= (term (fill (ring n_cap n_p n_c)))     ; fill never exceeds capacity
             (term n_cap)))])

;; Property: stepping a valid ring yields only valid rings.
;; (Preservation / "the protocol cannot be driven into an illegal state.")
(define (step-preserves-validity? r)
  (or (not (term (valid? ,r)))                      ; ignore junk start states
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

;; ============================================================
;; Run it.
;; ============================================================

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
