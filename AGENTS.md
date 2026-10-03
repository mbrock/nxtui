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

## Wisp Port

Incrementally preserve Wisp's language semantics, checking against the Zig
reference and documenting deliberate differences. Keep guest control state in
the heap. Use portable C++23 templates, concepts, and constexpr/consteval where
they make Zig's comptime-driven declarations equally concise and semantic;
derive metadata from types rather than maintaining parallel tables. Do not use
C++26 reflection until it is supported by mainline Clang as well as GCC.

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

When a sibling task needs to stop the main work, make the main work a job in
the same group, with a predicate that decides when to stop the others (for example
`nxtrt::settle(std::tuple{main, watcher}, nxtrt::primary_group{})`). A
group can only stop its own jobs; the task awaiting the group is not one of
them.

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
wish/exec, task, pool, or blocking-work semantics, update `nxtrt/runtime.rkt`
alongside the C++ code so the executable model keeps describing the runtime you
mean to have. The model currently focuses on exec lifecycle semantics; add wand
vocabulary only when modeling wand-level scheduling, ownership, or backend
capacity.

Run the model with:

```sh
nix develop -c make spec
```

`make spec` is pass/fail: every `run` block must be satisfiable (a witness that
cannot exist means the spec contradicts itself, which would make every property
vacuously true), and every `check` block must hold. A failing `checked` property
prints its counterexample as a step-by-step trace. `make spec-witnesses` prints
the example traces of the `run` blocks instead.

Checks read like run blocks:

```
check retire-only-when-ready :for ([1 ...] [2 exec task]) :trace-length 6
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

or, inside `nix develop`, directly with:

```sh
racket nxtrt/model.rkt --run-all
```

Racket packages and their compiled code live in the cacheable Nix
`spec-racket` derivation, never in the user's global Racket setup. Only
bytecode for editable repo sources lives in the version-keyed `.racket/`
cache (see `NXT_RACKET_CACHE_DIR` in the Makefile). Normal builds do not
resolve catalog packages; `nix/update-racket-sources.rkt` is the explicit
networked updater for `nix/racket-sources.json`.

## Build And Nix

Meson is the build system; the Nix flake (`flake.nix`, `nix/package.nix`)
only wraps it. `nix develop` gives the C++ and docs toolchain, including AWS-LC
for the crypto cross-check tests, plus the cached Racket dependencies and
Forge's JDK for runtime model work.
`nix build` builds the installable package with tests, and `nix flake check`
also checks the runtime specs offline and builds a small pkg-config consumer
(`nix/consumer.cpp`) against the install. When adding a public header
directory or a `.cpp` file under `src/`,
keep the header install excludes in `src/meson.build` in sync.

For wand bugs, first map the concrete operation to the model vocabulary:
`has-lifecycle`, `prepared-state`, `parked-state`, `settled-state`,
`retired-state`, `has-parked-phase`, `has-settled-phase`, `has-ready`,
`has-continuation`, `realizes`, and, for concurrent work, the pool's
`admitted`, `slots`, `free-slots`, `running-slots`, and `ready-slots`. Each exec
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

Tests are nested with the local DSL in `test/test.hpp`: `"name"_group = []
{ ... }` holds child groups and tests, and `"name"_test = [] { ... }` is a
leaf. The runner makes one pass: group bodies only declare children, test
bodies run only when selected, and results print as each test finishes. Keep
every test self-contained; siblings must not share state through their group.

A test that awaits runtime work can be a coroutine itself:
`"name"_test = []() -> nxtrt::task<void> { ... co_await ...; }` in a file that
includes `test/task-test.hpp`. The runner awaits it on a fresh deck through one
shared entry, so prefer this to a `deck.sync_wait([&] { ... })` per test; keep
an explicit deck only when the test drives or inspects the deck itself.

Slow integration and stress cases are marked `"name"_test.slow()` (or a whole
`.slow()` group). Everyday runs skip them; `build/nxt-tests --slow` runs
everything, `--only-slow` runs just them, and selecting a slow test by number
runs it. `meson test` runs the slow suites too, as CI does;
`meson test --no-suite slow` skips them. A test that needs more than one
second is a candidate for `.slow()` before it is a candidate for a longer
timeout.

Wisp tests that need the base library start from `wisp::test::base_image()`
in `test/wisp-base.hpp`: the base library is interpreted once per process
and every caller decodes a private copy of that machine.

Prefer writing Wisp behavior tests in Wisp. `test/wisp/*-test.wisp` files
use the harness in `test/wisp/harness.wisp` (`deftest`, `expect`,
`expect-equal`, and `:slow` after a test's name). `nxt-tests` reads them from
the source tree at run time under its WISP TEST FILES suite, one group per
file and one numbered test per `deftest`, each in a fresh machine with the
base library and `src/wisp/compiler.wisp` loaded from the source tree, so
editing either needs no rebuild. The CLI and `compiler_image()` embed the
compiler, so they see compiler changes only after a rebuild. Keep C++
tests for native APIs such as tapes, the heap, and the host.
