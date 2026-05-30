# nxtrt Racket ontology/model prototype

This directory is a small experiment in making the `nxtrt` domain a
single `#lang rdf-forge` source file.

- `runtime.rkt` declares the baseline ontology and model together.
- `runtime-next.rkt` is the double-buffered rewrite lane for reshaping the
  ontology/model while comparing against the baseline.
- `ontology.rkt` is a compatibility entrypoint that prints the ontology as
  Turtle/RDFS/OWL.
- `model.rkt` is a compatibility entrypoint that runs the model through
  Forge's functional API without generating `.frg` text.
- `ontology-next.rkt` and `model-next.rkt` are matching entrypoints for the
  next model.

Install the local language once in this checkout:

```sh
raco pkg install --auto --link -n rdf-forge rdf-forge
```

Generate a basic Turtle/RDFS/OWL view of the ontology:

```sh
racket nxtrt/ontology.rkt
```

Export a witness as Alloy XML:

```sh
racket nxtrt/model.rkt --direct-xml /tmp/nxtrt-direct.xml --run rich-runtime-shape-witness
```

Print a witness as readable text:

```sh
racket nxtrt/model.rkt --text --run rich-runtime-shape-witness
```

Print all model witnesses as readable text:

```sh
make spec
```

Run the structured checks:

```sh
racket nxtrt/model.rkt --check
```

Predicates in `runtime.rkt` are written in the indentation-sensitive
`rdf-forge` language rather than raw Forge text. For example:

```racket
predicate has-parked-exec
  some ([a exec])
    in (a has-lifecycle) parked-state
```

The ontology can name disjoint class partitions with `variant`. This expands
to an abstract parent and subclass children in the model, and exports an OWL
`disjointUnionOf` in Turtle:

```racket
variant exec-state (prepared-state parked-state settled-state retired-state)
```

Fields whose range is a variant parent also get an automatic model invariant
that keeps their values inside the declared case union. For example,
`has-lifecycle var one exec-state` carries both the `one` field multiplicity
and the `exec-state` variant-range constraint.

Model signatures can be derived from an ontology by including the ontology
value as a model part. Class terms become signatures, and property declarations
with a domain/range become fields:

```racket
ontology cards "https://example.test/cards#"
  class DECK
  class CARD
  class MOOD is DAWN or NOON or DUSK

  a DECK holds some CARD
  a DECK varyingly picks a CARD or not
  a CARD varyingly feels one MOOD

model cards-model
  cards
```

Quantified variables are callable inside predicate bodies. Calling a variable
with relation steps follows that path from the variable, so
`(a has-lifecycle)` is the same relation expression as
`(follow a has-lifecycle)`.

Forge expressions can use infix forms for the common logical/relation
operators:

```racket
/\ c ∈ d/holds
/\ c/tries = t
/\ c/feels ∈ NOON
/\ d/picks = c
```

`∧` and `∨` are accepted in the same positions as `/\` and `\/`.

The slash operator follows a relation path, so `d/holds` is the same relation
expression as `(d holds)`. The reader also accepts a small Unicode surface for
the same operators:

```racket
∀ d ∈ DECK, c ∈ d/picks:
  /\ c ∈ d/holds

∃ d ∈ DECK, c ∈ d/holds:
  /\ c/feels ∈ NOON
  /\ d/picks = c
```

Run blocks can be written as small scenarios with a scope, optional trace
length, and the predicates to show or hold:

```racket
run R1:
  exactly 1 of DECK, TASK, FLAP, DAWN, NOON, DUSK, DEAD
  show P2

run R2:
  exactly 1 of DECK, TASK, FLAP, DAWN, NOON, DUSK, DEAD
  for 2 steps
  always P1
  show P3
```

The next model also sketches the basic bthreads game semantics: a `GAME`
chooses a `CARD` only when some current `SYNC` asks for it and no current sync
blocks it; bthreads whose current sync asks or waits for that card advance to
the sync's `then` target in the next state.

These are equivalent to the prefix forms:

```racket
in c (d holds)
== (c tries) t
&& (in (c feels) noon) (== (d picks) c)
```

Field declarations resolve overloaded properties from the enclosing signature
domain, so a signature can stay close to the ontology wording:

```racket
signature firm
  spawned set task
  issued set deed
```

Use `follow` for outgoing relation paths and `matching` for inverse lookups:

```racket
all ([z firm] [t (z spawned)])
  lone ([d (z issued)])
    == (d observes) t
```
