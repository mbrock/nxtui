#lang racket
(require redex)

(provide Ring
         fill
         free
         ring->
         valid?
         step-preserves-validity?)

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

(module+ main
  (render-language Ring)
  (render-reduction-relation ring->)
  (render-metafunction fill)

  (stepper ring-> (term (ring 2 0 0))))
