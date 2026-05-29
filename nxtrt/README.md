# nxtrt Racket ontology/model prototype

This directory is a small experiment in making the `nxtrt` domain a
single `#lang rdf-forge` source file.

- `runtime.rkt` declares the ontology and model together.
- `ontology.rkt` is a compatibility entrypoint that prints the ontology as
  Turtle/RDFS/OWL.
- `model.rkt` is a compatibility entrypoint that runs the model through
  Forge's functional API without generating `.frg` text.

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

Quantified variables are callable inside predicate bodies. Calling a variable
with relation steps follows that path from the variable, so
`(a has-lifecycle)` is the same relation expression as
`(follow a has-lifecycle)`.

Field declarations resolve overloaded properties from the enclosing signature
domain, so a signature can stay close to the ontology wording:

```racket
signature zone
  spawned set task
  issued set deed
```

Use `follow` for outgoing relation paths and `matching` for inverse lookups:

```racket
all ([z zone] [t (z spawned)])
  lone ([d (z issued)])
    == (d observes) t
```
