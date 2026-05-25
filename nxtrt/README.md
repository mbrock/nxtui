# nxtrt Racket ontology/model prototype

This directory is a small experiment in making the `nxt::rt` Forge model a
pair of Racket modules.

- `ontology.rkt` declares the `nxt` ontology and provides top-level bindings
  such as `task`, `zone`, `belongs-to`, and `parked-on`.
- `model.rkt` imports those bindings, builds a Forge model value, and runs it
  through Forge's functional API without generating `.frg` text.

Generate a basic Turtle/RDFS/OWL view of the ontology:

```sh
racket nxtrt/ontology.rkt
```

Export a witness as Alloy XML:

```sh
racket nxtrt/model.rkt --direct-xml /tmp/nxtrt-direct.xml --run rich-runtime-shape-witness
```

Run the structured checks:

```sh
racket nxtrt/model.rkt --check
```

Predicates in `model.rkt` are written as Racket data expressions rather than
raw Forge text. For example:

```racket
(all ([a waiter])
  (=> (some (follow a parked-on))
      (== (follow a (belongs-to (task waiter)) (belongs-to waiter)) a)))
```

Field declarations resolve overloaded properties from the enclosing signature
domain, so a signature can stay close to the ontology wording:

```racket
(signature task
  (field belongs-to #:lone zone)
  (field belongs-to #:lone deck)
  (field belongs-to #:lone waiter)
  (field continues-as #:lone task))
```

Use `follow` for outgoing relation paths and `matching` for inverse lookups:

```racket
(all ([z zone] [t (matching (belongs-to (zone task)) z)])
  (lone ([d (matching (belongs-to (zone deed)) z)])
    (== (follow d (belongs-to (task deed))) t)))
```
