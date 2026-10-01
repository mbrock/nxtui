# Repository Notes

## Delivery Workflow

For completed work tasks, verification, committing, and pushing to the current
branch's established upstream are the default. The user authorizes this normal
delivery workflow without a separate pre-commit or pre-push review gate, unless
they ask to keep a particular task local. Commits track ongoing work; they are
not releases or promises of a finished version. Make later corrections or
reverts as new commits rather than waiting for approval to record progress.

Fetch and integrate upstream changes as needed. Rebase unpublished local
commits or merge as appropriate, preserve concurrent work, and rerun affected
checks after resolving conflicts. Do not include unrelated worktree changes.
This authorization does not cover force-pushing or rewriting published history,
publishing releases, deploying, or other destructive/shared-state operations.
Report what was verified and whether the work was actually committed and pushed;
do not call local-only work delivered if a push remains blocked.

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
a forked child owned by that scope. A firm body is not automatically the same
thing as one of the firm's child deeds.

Do not paper over freezes by repeatedly running the whole test suite. Reproduce
the failing app path, inspect the parked tasks/wishes, and fix the concrete
runtime or lifetime bug.

## Runtime Model

The executable `nxtrt` runtime/exec model lives in `nxtrt/runtime.rkt`. It uses
`#lang rdf-forge` to keep the RDF ontology vocabulary and the Forge-style model
in one source file: classes and properties name the runtime concepts,
signatures describe their shape, predicates state invariants or temporal
expectations, and `run` blocks are small witness/debugging scenarios.

`nxtrt/model.rkt` is only the CLI wrapper for that model, and
`nxtrt/ontology.rkt` is the ontology export wrapper. When changing deck,
wish/exec, task, deed, or firm semantics, update `nxtrt/runtime.rkt`
alongside the C++ code so the executable model keeps describing the runtime you
mean to have. The model currently focuses on exec lifecycle semantics; add wand
vocabulary only when modeling wand-level scheduling, ownership, or backend
capacity.

Run the model with:

```sh
nix develop .#spec -c make spec
```

`make spec` is pass/fail: every `run` block must be satisfiable (a witness that
cannot exist means the spec contradicts itself, which would make every property
vacuously true), and every `check` block must hold. A failing `checked` property
prints its counterexample as a step-by-step trace. `make spec-witnesses` prints
the example traces of the `run` blocks instead.

Checks read like run blocks:

```
check retire-only-when-ready :for ([1 ...] [2 exec task deed]) :trace-length 6
  assume execs-start-prepared
  assume always structural-invariants
  assume always lifecycle-transitions
  show always retires-only-when-ready
```

`assume` lines are premises and `show` lines are claims; without an `expect`
line the claims must hold in every trace allowed by the premises, and
`:expect sat` / `:expect unsat` (or an `expect sat` line in block form) ask for
satisfiability instead. Comments in `#lang rdf-forge` start with `//`.

Phase fields are `lone`, so `in (a has-settled-phase) draining-phase` is
vacuously true when the exec has no settled phase. Test phase membership with
`some (intersect (a has-settled-phase) draining-phase)`. When adding a property,
check that it can fail: delete the rule it depends on and confirm `make spec`
reports it.

or, inside `nix develop .#spec` after one `make spec` has set up `.racket/`,
directly with:

```sh
racket nxtrt/model.rkt --run-all
```

Racket packages and compiled code for the spec live in the project-local,
version-keyed `.racket/` dir (see `NXT_RACKET_ADDON_DIR` in the Makefile),
never in the user's global Racket setup.

## Build And Nix

Meson is the build system; the Nix flake (`flake.nix`, `nix/package.nix`)
only wraps it. `nix develop` gives the C++ and docs toolchain, including AWS-LC
for the crypto cross-check tests. Racket and Forge's JDK are opt-in via
`nix develop .#spec`; basic orb setup does not install spec dependencies.
`nix build` builds the installable package with tests, and `nix flake check`
also builds a small pkg-config consumer (`nix/consumer.cpp`) against the
install. When adding a public header directory or a `.cpp` file under `src/`,
keep the header install excludes in `src/meson.build` in sync.

For wand bugs, first map the concrete operation to the model vocabulary:
`has-lifecycle`, `prepared-state`, `parked-state`, `settled-state`,
`retired-state`, `has-parked-phase`, `has-settled-phase`, `has-ready`,
`has-continuation`, `realizes`, `spawned`, `issued`, and `observes`. Each exec
owns its lifecycle and backend phase details. If the bug is a missing invariant
or impossible transition, encode that in the model before or alongside the
runtime fix.

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
