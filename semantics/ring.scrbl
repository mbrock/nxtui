#lang scribble/manual

@(require redex/pict
          "ring.rkt")

@(define (unquoted->plain lw)
   (set-lw-unq?! lw #f)
   lw)

@(define ((infix op) lws)
   (define lhs (list-ref lws 2))
   (define rhs (list-ref lws 3))
   (list "" lhs op rhs ""))

@(define (conjunction lws)
   (define lhs (list-ref lws 2))
   (define rhs (list-ref lws 3))
   (list "" lhs " and " rhs ""))

@(define (positive lws)
   (define value (list-ref lws 2))
   (list "" value " > 0" ""))

@(define-syntax-rule (typeset body)
   (with-unquote-rewriter
    unquoted->plain
    (with-compound-rewriters
     (['+ (infix " + ")]
      ['- (infix " - ")]
      ['<= (infix " <= ")]
      ['and conjunction]
      ['positive? positive])
     body)))

@title[#:version "" #:date ""]{Ring Buffer Semantics}

This note typesets the executable Redex model in @racket[ring.rkt].  A ring
state is written as @racket[(ring capacity produced consumed)]. The cursors are
monotonic; occupancy is derived by subtracting the consumed cursor from the
produced cursor.

@bold{State language.}
The model keeps the ring state deliberately small: capacity, producer cursor,
and consumer cursor.

@(render-language Ring)

@bold{Derived quantities and invariant.}
The @racket[fill] and @racket[free] metafunctions compute occupancy and
available capacity; @racket[valid?] states @racket[0 <= fill <= capacity].

@(typeset (render-metafunctions fill free valid? #:contract? #t))

@bold{Transitions.}
Producing advances the producer cursor when free space remains. Consuming
advances the consumer cursor when the ring is non-empty.

@(typeset (render-reduction-relation ring->))

@bold{Executable check.}
The model test runs @racket[redex-check] for 5000 attempts, searching for a
one-step counterexample to validity preservation.
