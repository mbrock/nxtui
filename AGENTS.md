# Repository Notes

## Coroutine Wisdom

COROUTINE LAMBDAS THAT CAPTURE WILL CAUSE SEGFAULTS AND VERY ANNOYING
ISSUES!!!

Do not make a capturing lambda whose `operator()` is itself a coroutine and
then let the returned task outlive the lambda object. The coroutine frame does
not save the lambda closure for you; captured references/state can dangle and
produce crashes, stuck timer loops, corrupted UI state, and deeply misleading
debugging sessions. Prefer a named coroutine helper function or pass state as
explicit coroutine parameters.

If a UI/tool animation is "just waiting on timers forever", first suspect a
lifetime or completion-signal bug, not the timer. Verify that the worker task
can actually set the `done` flag it is supposed to set.

When a sibling task needs to stop the main work in a scope, make the main work
a forked child owned by that scope. A zone body is not automatically the same
thing as one of the zone's child deeds.

Do not paper over freezes by repeatedly running the whole test suite. Reproduce
the failing app path, inspect the parked tasks/wishes, and fix the concrete
runtime or lifetime bug.

## Runtime Model

The executable `nxtrt` runtime/wand model lives in `nxtrt/runtime.rkt`. It uses
`#lang rdf-forge` to keep the RDF ontology vocabulary and the Forge-style model
in one source file: classes and properties name the runtime concepts,
signatures describe their shape, predicates state invariants or temporal
expectations, and `run` blocks are small witness/debugging scenarios.

`nxtrt/model.rkt` is only the CLI wrapper for that model, and
`nxtrt/ontology.rkt` is the ontology export wrapper. When changing deck, wand,
wish/exec, task, deed, or task-zone semantics, update `nxtrt/runtime.rkt`
alongside the C++ code so the executable model keeps describing the runtime you
mean to have.

Run the model with:

```sh
make spec
```

or directly with:

```sh
racket nxtrt/model.rkt --run-all
```

For wand bugs, first map the concrete state to the model vocabulary:
`has-prepared`, `has-submitted`, `has-parked`, `has-ready`,
`has-continuation`, `realizes`, `spawned`, `issued`, and `observes`. If the bug
is a missing invariant or impossible transition, encode that in the model
before or alongside the runtime fix.

## Tests

Run the main test binary directly to see the nested test report:

```sh
build/nxt-tests
```

The report numbers every nested test, and you can select tests or whole
subtrees by number:

```sh
build/nxt-tests 1 2.7 7
```

Tests are nested with the local `_test` DSL in `test/test.hpp`.
