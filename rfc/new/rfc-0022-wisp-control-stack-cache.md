# RFC 0022: Wisp Control Stack Cache {#rfc_wisp_stack_cache}

Status: proposed.

## Proposal

Keep the youngest continuation frames of a running Wisp run in a small,
fixed-capacity native cache, and write them to heap `ktx` rows only when
something can observe them. Run several transitions per native evaluator
activation instead of reading and writing the run row around every one.

Guest control state stays in the heap, as the project requires: at every
point where the host, the collector, a tape, or guest reflection can look,
the heap holds the complete continuation, exactly as it does today. Between
those points, frames that are pushed and popped again never exist as heap
rows.

This is the classic stack/heap strategy for first-class continuations
(Clinger, Hartheimer, and Ost's survey; Scheme48's stack cache), close to
Smalltalk's context-to-stack mapping (Deutsch and Schiffman), with reinstated
frames copied back lazily as in Hieb, Dybvig, and Bruggeman's segmented
stacks. It is independent of the code representation: source interpretation
and lowered code (@ref rfc_wisp_lowered_code "RFC 0021") push the same frames
through the same paths and both benefit.

## Why

Every pending operation is a heap row. TAK pushes 365,754 frames per run in
source mode and 254,437 lowered, and allocates about 1.1 million and 764,000
word-pool words, much of it call progress vectors. Almost all of these die
within a few transitions, but each is a table row and payload slice that
lives until the next collection, so frames are a major source of collection
pressure as well as allocation work.

The per-transition boundary costs too. In the native profile recorded in
RFC 0021, `evaluator::step` itself was 19–32% of self time: each transition
reads the six-column run row, builds the step state, enters a `try`, writes
the row back, and checks `status` several times. The interpreter is bound by
instruction count, so this fixed cost matters on every transition.

## How control works today

A run has two continuation registers (`src/wisp/eval.cpp`, around
`control_context`):

- `way`, the current segment: a linked list of ordinary `ktx` frames
  (`hop env fun acc arg`) ending at `top`;
- `meta`, a linked list of boundary frames (`PROMPT`, `BINDING`, `RESUME`),
  each parking the outer segment in its `arg`.

Capture copies only the boundary prefix and shares ordinary frames. It sets
a watermark (`freeze_continuations`) at the current size of the `ktx` table:
rows below it are frozen, and `writable_frame` copies a frozen frame, and its
progress vector, before the first write. Collection and tape restore freeze
every survivor.

Two consequences matter here. A frame above the watermark is reachable only
from its own run's continuation, through `way` or a boundary in `meta` that
parked it: anything that could share it, such as capture or reflection, would
have frozen it. Stage 1 should assert this rather than assume it.
And collection runs only between `advance` calls (`GC` sets a flag that stops
`advance`), so heap words held natively during a batch cannot move.

## Requirements

1. **Observational equivalence.** Wherever the heap can be observed, it
   holds exactly the frames, progress, and boundaries the all-heap evaluator
   would have produced. Multi-shot resumption, shared lexical locations, and
   per-resumption progress keep their current semantics.
2. **Heap-only control at rest.** When `advance` returns, the cache is empty
   and the run row is current. Tapes, the collector, and the host never see
   native control state.
3. **No native allocation during evaluation.** The cache has fixed capacity,
   allocated with the evaluator. A batch that overflows it spills its oldest
   frames to the heap. The allocation-failure test's guarantee, that small
   evaluations need only existing guest capacity, still holds.
4. **Atomic transitions.** A guest condition or allocation failure leaves
   the run at the last completed transition boundary, as now. Writing the
   cache back to the heap must not fail halfway: reserve table and pool
   capacity first, then write.
5. **One frame interface.** The executor reads and writes frames only
   through a small interface (top frame, push, pop, set position, update
   progress), so whether the top frame is cached or a heap row is invisible
   to the transitions of both execution modes.

## Design

### The cache

The evaluator owns a fixed array of cached frames, each holding the frame
columns natively: `env`, `fun`, `acc`, `arg`. The cache is a stack segment
sitting on top of the heap segment: its bottom frame's `hop` is the run's
`way` as last written. `push` appends to the cache; returning to a frame pops
it. When the cache is empty and a transition needs the top frame, it reads
the heap row at `way`.

### Underflow: pulling a heap frame up

A transition that only reads the top heap frame and pops it (a branch, a
sequence's last form, a single-argument call) needs nothing more. A
transition that writes it first moves it into the cache: the cached copy
replaces it, and `way` becomes its `hop`. If the heap row is frozen, this is
exactly today's copy-on-write. If it is not, the row was reachable only from
this run, so moving it is safe and the old row becomes garbage. Either way,
writes then happen natively. This is the underflow handler of segmented
stacks: reinstated frames come back lazily, one at a time.

### Flush points

Writing the cache back means creating `ktx` rows from bottom to top, linking
each to the previous `way`, and setting `way` to the top one. Frames written
back are ordinary rows: below the watermark if a capture then freezes them,
above it otherwise. Flush:

- when the batch ends: budget exhausted, the run completes or fails, `GC`
  requests collection, or `STEP!` switches to another run;
- before capture or any boundary change: `get/cc`, `send!` and therefore
  `await`, `compose-continuation`, `call-with-prompt`, `call-with-binding`,
  and condition signaling, which sends `ERROR`;
- before any builtin that reads control or a run: the `KTX-*` accessors,
  `TOP?`, `RUN-WAY`, `RUN-EXP`, `RUN-VAL`, `RUN-ERR`, and `STEP!`;
- when the cache is full: spill the oldest frames, keep the youngest.

The set of observing builtins is a property of each builtin, declared in the
builtin table next to its existing `control` flag, not a separate list.
Lexical environments are untouched: they are already shared heap storage, so
`ENV` and source escapes need no flush.

### Batching transitions

`evaluator::step` reads the run row once, runs transitions with the
registers and cache held natively until a flush point or the budget, then
writes the row once. `advance` passes its budget down instead of calling
`step` per transition. `STEP!` chains keep their current semantics: switching
runs ends the batch for the current run. GC polling still happens at most
every budget's worth of transitions.

Transition atomicity needs a small undo record: before each transition, the
registers, the cache depth, and a copy of the top cached frame, since one
transition mutates at most the top frame and pushes or pops a bounded
number. On a guest condition or allocation failure, restore it, flush, and
proceed as now.

### Inline progress (second stage)

Call and `LET` frames keep their partial values in heap vectors, which are
most of the word-pool allocation. A cached frame can keep them in a native
value stack beside the cache instead, materializing a `v32` only when the
frame is flushed and copying it back when the frame is pulled up. Source
frames' argument progress gets the same treatment. This is a separate stage,
because it changes more of the transition code than the cache itself.

## Stages and acceptance

### 1. A frame interface

Route every frame access in `eval.cpp` through the interface of requirement
5, with heap rows as the only implementation. No behavior or performance
change. Acceptance: all tests pass; no transition touches `way` or `ktx`
columns directly.

### 2. Batched transitions

Hold the registers natively across a batch, with flush points and the undo
record, but no frame cache yet. Acceptance: all tests, including allocation
failure and every slow suite, pass; `evaluator::step`'s share of the native
profile falls; TAK and the micro cases are measured in both modes.

### 3. The frame cache

Add the fixed cache, underflow, spill, and flushes. Acceptance:

- the corpus and lowered tests pass; multi-shot tests pass with collection
  after every transition; tapes taken at every flush point round-trip;
- a stress test fills and spills the cache with deep non-tail recursion,
  captures in the middle, and resumes twice;
- new profile counters report cached pushes, frames flushed by reason, frames
  pulled up, and spills; heap frame pushes on TAK fall by at least an order
  of magnitude;
- timings and collection counts for all benchmarks, in source and lowered
  modes, against the preceding commit.

### 4. Inline progress

Acceptance: word-pool words per TAK run fall substantially, the effect and
router benchmarks (which capture and resume) do not regress beyond noise,
and progress-copy counts stay correct.

## Open questions

- Cache capacity: large enough that ordinary recursion rarely spills, small
  enough that a flush is cheap. Measure spill counts on the benchmarks
  rather than guess.
- Whether `RESUME` and other boundary frames should be cacheable. Starting
  with "boundaries always flush" is simpler and keeps `meta` entirely in the
  heap.
- Whether condition signaling is frequent enough in real programs that
  flushing on every `fail` matters. Conditions caught locally might be
  handled without a full flush later.
- How much of the remaining per-transition cost is the step state itself
  once batching exists, and whether `eval_step` should then live for a whole
  batch.
