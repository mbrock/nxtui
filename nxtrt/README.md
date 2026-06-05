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

On a fresh machine, install the ordinary Racket catalog dependencies and link
the vendored Racket packages once:

```sh
make setup-racket
```

The `forge` and `something` packages used by this model are vendored under
`vendor/racket/` because this repo depends on small local patches to both.
`make spec` prepends the vendored collections to `PLTCOLLECTS`, so it does not
depend on sibling checkouts in `~/src`.

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

`make spec` also runs the BFO-inspired `rdf-forge` ontology syntax smoke test
in `rdf-forge/tests/bfo-sketch-test.rkt`.

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

BFO also uses explicit pairwise class disjointness. Use `disjoint` when the
classes are already declared and should export `owl:disjointWith` without
creating a variant parent:

```racket
class continuant :subclass-of entity
class occurrent :subclass-of entity
disjoint continuant occurrent
```

For BFO-style defined parent classes, `equivalent-union` exports an
`owl:equivalentClass` whose body is an `owl:unionOf` list:

```racket
equivalent-union entity continuant occurrent
equivalent-union continuant dependent-continuant independent-continuant spatial-region
```

`equivalent-intersection` is the matching class-list form for OWL
`intersectionOf` definitions:

```racket
equivalent-intersection material-entity independent-continuant continuant
```

For class axioms that say every instance has some relation to a filler class,
`subclass-some` exports an OWL existential restriction:

```racket
subclass-some independent-continuant located-in spatial-region
```

Use `subclass-only` for the universal restriction shape that appears in BFO
OWL class axioms:

```racket
subclass-only material-entity has-part material-entity
```

Class and property blocks can carry lightweight annotation triples too. The
short names map to common RDF/SKOS predicates, matching the label/example/
elucidation-heavy shape of BFO term declarations:

```racket
class entity
  label "entity"
  pref-label "entity"
  elucidation "An entity is anything that exists, has existed, or will exist."

property part-of
  label "part of"
  transitive
```

`label` exports as `rdfs:label`, `pref-label` and `alt-label` export as
SKOS labels, `definition` and `elucidation` export as `skos:definition`,
and `example` exports as `skos:example`. For local predicates, use
`annotation name "value"`.

Annotation predicates can be declared too, which mirrors the BFO header:

```racket
annotation-property bfo-owl-label
  label "BFO OWL specification label"
  subproperty-of label

annotation-property bfo-clif-label
  label "BFO CLIF specification label"
  subproperty-of label

annotation-property has-associated-axiom-fol
  label "has associated axiom(fol)"
```

BFO also carries Common Logic/FOL material in the OWL as annotations. Its
`fol-mungall` sources use readable CLIF meta-axioms such as
`(transitive part_of)`, then expand them to first-order formulas for theorem
provers. The ontology syntax mirrors that habit with `clif-label`,
`nl-axiom`, and `fol-axiom` annotation shortcuts:

```racket
class continuant :subclass-of entity
  clif-label "Continuant"
  nl-axiom "if b is a continuant, then b is an entity. (axiom label in BFO2 Reference: [008-002])"
  fol-axiom "(forall (x) (if (Continuant x) (Entity x))) // axiom label in BFO2 CLIF: [008-002]"
```

When an axiom deserves its own stable label, use `logical-axiom`. This emits
the subject's direct `has-associated-axiom-*` triples and an `owl:Axiom`
annotation node with `has-axiom-label`, matching the way BFO connects CLIF
text to reference labels:

```racket
logical-axiom axiom-008-002 continuant
  label "BFO2 Reference [008-002]"
  nl "if b is a continuant, then b is an entity."
  fol "(forall (x) (if (Continuant x) (Entity x)))"
```

`rdf-forge/bfo-sketch.rkt` ports a small representative sample of these BFO
logical axioms into the syntax: subclass implications, existential commitments,
biconditional definitions, and relation-level CLIF meta-axioms like
`(transitive part_of)` and `(inverse_of has_part part_of)`.

It also sketches BFO parthood in two layers. The exact BFO axioms stay as
named CLIF/FOL annotations, especially where BFO uses the time-indexed ternary
`continuantPartOfAt(a, b, t)`. Binary shadows such as `continuant-part-of`,
`occurrent-part-of`, `proper-continuant-part-of`, and
`proper-occurrent-part-of` give the executable Forge model a tractable
fragment for transitivity, inverse, chain, and range-closure constraints:

```racket
property continuant-part-of
  continuant set continuant
  transitive
  reflexive
  fol-axiom "(forall (x y z t) (if (and (continuantPartOfAt x y t) (continuantPartOfAt y z t)) (continuantPartOfAt x z t)))"

logical-axiom axiom-009-002 continuant
  label "BFO2 Reference [009-002]"
  nl "if b is a continuant and, for some t, c is continuant_part_of b at t, then c is a continuant."
  fol "(forall (x y) (if (and (Continuant x) (exists (t) (continuantPartOfAt y x t))) (Continuant y)))"
```

Some ontology axioms also generate executable Forge constraints when an
ontology is included in a `model`. The generated fragment currently covers
property transitivity, inverse properties, property chains, class union and
intersection definitions, and `subclass-some`/`subclass-only` restrictions.
The original RDF/CLIF annotations are still exported for ontology tooling,
while the model gets hidden generated predicates that are applied to runs and
checks with the same mechanism as field multiplicity constraints.

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

Property blocks can also carry OWL-style property characteristics. This is
the direction sketched by BFO/RO ontologies, where object properties such as
part relations are often transitive and adjacency-style relations can be
symmetric:

```racket
property part-of
  transitive

property adjacent-to
  symmetric

property has-part
  transitive
  inverse-of part-of

property located-in
  continuant set spatial-region
  transitive
  subproperty-of related-to

property-chain located-in (part-of located-in)
```

These export as `owl:TransitiveProperty`, `owl:SymmetricProperty`,
`owl:inverseOf`, `rdfs:subPropertyOf`, and `owl:propertyChainAxiom`, plus the
usual `rdfs:domain` and `rdfs:range` triples when a domain/range clause is
present. See
`rdf-forge/bfo-sketch.rkt` for a small BFO-inspired syntax smoke test.

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
/\ no ((halts ∋ c) ∩ (g/runs/awaits))
```

`∧` and `∨` are accepted in the same positions as `/\` and `\/`.

The slash operator follows a relation path, so `d/holds` is the same relation
expression as `(d holds)`. `∩`/`&` intersect relations, and `relation ∋ value`
or `relation ~ value` does an inverse lookup, equivalent to
`(matching relation value)`. The reader also accepts a small Unicode surface
for the same operators:

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
no (intersect (matching halts c) (g runs awaits))
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
