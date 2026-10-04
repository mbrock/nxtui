# RFC 0022: Wisp Control Stack Cache {#rfc_wisp_stack_cache}

Status: implemented, including inline progress. The stage 4 timing
non-regression criterion is not fully met; see measurements below.

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

Before this change, every pending operation was a heap row. TAK pushed
365,754 frames per run in source mode and 254,437 lowered, and allocated about
1.1 million and 764,000 word-pool words, much of it call progress vectors.
Almost all of these died within a few transitions, but each was a table row
and payload slice that lived until the next collection, making frames a major
source of collection pressure as well as allocation work.

The per-transition boundary costs too. In the native profile recorded in
RFC 0021, `evaluator::step` itself was 19–32% of self time: each transition
reads the six-column run row, builds the step state, enters a `try`, writes
the row back, and checks `status` several times. The interpreter is bound by
instruction count, so this fixed cost matters on every transition.

## Heap control representation

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
have frozen it. Writes to heap frames assert that they are not frozen.
And collection runs only between `advance` calls (`GC` sets a flag that stops
`advance`), so heap words held natively during a batch cannot move.

## Requirements

1. **Observational equivalence.** Wherever the heap can be observed, it
   holds exactly the frames, progress, and boundaries the all-heap evaluator
   would have produced. Multi-shot resumption, shared lexical locations, and
   per-resumption progress keep their current semantics.
2. **Heap-only control at rest.** When `step` or `advance` returns normally,
   including a failed run after an unhandled guest condition, the cache is
   empty and the run row is current. Tapes, the collector, and the host never
   need native control state. Host panic unwinding is not a resumable return.
3. **No native allocation to manage the cache.** It has fixed capacity,
   allocated with the evaluator. A batch that overflows it spills its oldest
   frames to the heap. The allocation-failure test's guarantee, that small
   evaluations need only existing guest capacity, still holds.
4. **Preserve failure semantics, without rollback.** Guest conditions are
   sent to `ERROR` using the current control state, as today; neither native
   registers nor guest heap mutations are rewound. Host allocation failure
   is a panic: the C++ exception escapes, and the interrupted machine is not
   promised to be resumable or safe to retry. This includes allocation
   failure while materializing cached frames. Panic unwinding must not try
   to allocate more memory to reconstruct the continuation.
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

The implementation holds 64 frames. Frame overflow spills the oldest half;
progress overflow can spill all ancestors of the current frame. Capacity is
an implementation detail, not a limit on guest continuation depth.

### Underflow: pulling a heap frame up

A transition that only reads the top heap frame and pops it (a branch, a
sequence's last form, a single-argument call) needs nothing more. A
transition that writes a frozen frame first copies it into the cache, and
`way` becomes its `hop`. Its mutable progress must also be copied, as in
today's copy-on-write: copying only the frame columns would still share the
progress vector. Reinstated frozen frames come back lazily, one at a time.

A writable heap row stays in place. Although it is not shared with another
continuation, the transition-entry run row still refers to it: a reflective
callee can inspect its own run after the call frame has been popped and see
that row's completed argument progress. Writing through preserves that
identity and avoids creating another row at the next flush.

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

Run reflection preserves the transition-entry view, not halfway-updated
registers. Each transition remembers its entry registers and the cached
prefix they name; spills update that prefix's heap link. If the transition
pops its top cached frame, retain it until the transition ends and only
materialize it if an observing builtin needs the entry view. This is an
observation snapshot, not an undo record: in-place progress changes remain
visible, and nothing is rewound.

### Batching transitions

`step` and `advance` share `evaluator::execute`, which reads the run row once,
runs transitions with one `eval_step` state held natively for the batch, and
writes the row on return or a run switch. `step` requests one transition;
`advance` passes its budget down instead of calling `step` repeatedly.
Observing builtins additionally publish the transition-entry view. `STEP!`
chains remain iterative: switching runs commits the current run, and each
nested target takes exactly one transition. `advance` stops on a pending GC
request; explicit `step` retains its previous ability to step anyway.

There is no transition undo record. On a guest condition, materialize the
current continuation and deliver `ERROR`, preserving the existing condition
semantics. On a host allocation exception, abandon the batch without a
flush or rollback attempt. The host may catch the exception to dispose of
the machine, not to resume it.

### Inline progress

Source and lowered call frames, and lowered `LET` frames, keep partial values
in a 1,024-word native stack beside the cache. Each frame records its slice;
pop reclaims that slice, and spill materializes a `v32` then compacts the
remaining slices. Pulling a frozen frame copies its progress into this stack.
A vector larger than the entire native stack uses the existing heap path.
Source `LET` keeps its existing cons accumulator.

A separate 1,024-word scratch array preserves the popped transition-entry
frame's progress for a reflective callee, allowing its main slice to be
reused immediately. Neither array contains roots or tape state at rest. This
was implemented separately from the frame cache so its allocation and timing
effects could be measured independently.

## Stages and acceptance

### 1. A frame interface

Route every frame access in `eval.cpp` through the interface of requirement
5, with heap rows as the only implementation. No behavior or performance
change. Acceptance: all tests pass; no transition touches `way` or `ktx`
columns directly.

### 2. Batched transitions

Hold the registers natively across a batch, with observation points and the
existing guest-condition delivery, but no frame cache yet. Acceptance: all
tests, including allocation failure and every slow suite, pass;
`evaluator::step`'s share of the native profile falls; TAK and the micro
cases are measured in both modes.

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

## Implementation checks and measurements (2026-10-04)

All four stages were committed separately. The final Clang 23 assertion-enabled
build passed all 18 Meson cases, including slow suites: 14 passed and four
invalid benchmark invocations failed as expected. GCC 16 passed 256 Wisp tests,
the explicit deep-cache stress case, all five allocation tests, and the semantic
counter check. Profiling-enabled and disabled counter builds both passed.

New checks cover self-reflection of writable and frozen argument progress,
nested `STEP!` observers, tape round-trips after every batch, and 129-deep
source/lowered capture followed by two resumptions of each of two restored
tapes. The deep test collects between batches of 1, 17, and 4,096 transitions.
Counter tests force frame and progress spills, check both sides of the
1,024-word capacity boundary, and check exact frozen-progress copying.
Fault injection verifies that allocation panic makes no further allocation
attempt during unwinding; the interrupted machine is discarded, not retried.

The [raw timing samples](../../bench/wisp/results/2026-10-04-stack-cache.jsonl)
contain four full sweeps (20 cases × two modes × three samples), a longer
90-sample interleaved comparison, and a 45-sample accessor-inlining experiment
that was rejected. All results were checked. Timing builds use the locked
Clang 23 toolchain, Meson release, assertions and semantic counters disabled,
CPU 0, and the existing 4,096-transition GC policy. Runs were serial, with no
builds or tests competing. Exact commands, revisions, hashes, and build options
are in the full-sweep environment records. `lowered-library` lowers both the
workload and the base library; router cases are guest-continuation workloads,
not HTTP throughput.

TAK evaluator-only median milliseconds per run in the full sweeps:

| Stage | Source | Lowered library |
| --- | ---: | ---: |
| Original evaluator | 65.220 | 47.110 |
| Batched transitions | 45.740 | 37.306 |
| Frame cache | 44.720 | 34.082 |
| Inline progress | 55.229 | 37.650 |

Elapsed times varied substantially in this orb, so the later comparison
rotated adjacent original/frame-cache/inline-progress runs with longer timed
regions: TAK 20 iterations, shallow effects 200,000, deep effects and routers
10,000. Median paired inline-progress/frame-cache time ratios were:

| Workload | Source | Lowered library |
| --- | ---: | ---: |
| TAK | 1.064 | 1.094 |
| Shallow effects | 0.931 | 0.967 |
| Deep effects | 0.935 | 0.996 |
| Router hit | 1.017 | 1.021 |
| Router miss | 1.122 | 1.021 |

Thus the allocation reduction is established, but the inline-progress stage
is not an across-the-board timing win. Source router misses were slower than
the frame-only cache, including in a subsequent five-pair comparison.
Forcing the progress accessor inline did not consistently help and was
reverted. Inline progress is retained for its allocation reduction; the stage 4
non-regression criterion remains open rather than being reported as passed.
The complete implementation was faster than the original evaluator on all five
workloads in both modes in the longer interleaved comparison.

Separate [semantic diagnostics](../../bench/wisp/results/2026-10-04-stack-cache-profile.jsonl)
measure one TAK iteration, without warmup (their instrumented elapsed times
are not timing evidence). Inline diagnostics were built from the implementation
subsequently committed as the inline-progress stage; their embedded revision
still names the preceding frame-cache commit with `-dirty`.

| TAK metric, source / lowered library | Original | Frame cache | Inline progress |
| --- | ---: | ---: | ---: |
| Heap `ktx` allocations | 365,928 / 254,538 | 2,556 / 1,574 | 2,546 / 1,569 |
| Word-pool words allocated | 1,097,649 / 763,710 | 1,097,437 / 763,534 | 517,619 / 514,378 |
| Collections | 14 / 8 | 7 / 4 | 4 / 3 |

Logical pushes remain 365,754 / 254,437, while native batches are 358 / 202.
TAK never spills the 64-frame cache. Deep effects, recursive division, and
list workloads do spill, as expected; the capacity is not a universal
no-spill claim. Progress-copy counters vary with collection frequency, so the
exact-copy unit test checks their meaning independently of these totals.

[Native sampling](../../bench/wisp/results/2026-10-04-stack-cache-perf.txt)
used `perf record -e cpu-clock:u -F 997`, TAK 100 iterations and three warmups,
in each mode. Hardware cycles/instructions were unavailable. Samples cover
the whole process, including setup. Original `evaluator::step` self time was
26.01% / 18.20%; the batched replacement `execute` was 7.58% / 7.79%.
The completed cache/progress implementation was 13.72% / 13.59%, reflecting
additional bookkeeping and a different distribution of remaining work.

## Open questions

- Whether `RESUME` and other boundary frames should be cacheable. Starting
  with "boundaries always flush" is simpler and keeps `meta` entirely in the
  heap.
- Whether condition signaling is frequent enough in real programs that
  flushing on every `fail` matters. Conditions caught locally might be
  handled without a full flush later.
