# RFC 0021: Wisp Lowered Code {#rfc_wisp_lowered_code}

Status: compact-node lowering and execution implemented, opt-in. Record
execution is retired (stage 4); source execution is the reference. Code
records are immutable and checked once, which made fully lowered execution
faster than source on every benchmark. Flat code and per-activation frames
remain deferred.

## Proposal

Lower checked semantic records into a separate executable representation, and
run that instead of the records. @ref rfc_wisp_semantic_code "RFC 0020" built
the compiler's record IR and executed it directly; this RFC keeps that IR as
the semantic representation and designs what replaces it at run time.

The question is narrow: how do we erase the compiler representation while
preserving everything execution needs, including continuations that are
captured, collected, saved to tape, and resumed more than once? An opcode
table is the least interesting part of the answer. The central part is what a
suspended frame refers to once the IR graph is gone.

The proposal is to start with the smallest representation that answers it:
**compact executable nodes**, one small word vector per operation, in which a
continuation point is still a single heap word. Frames, capture, collection,
and tapes then keep their present shape. A flat instruction vector with
program counters, and one frame per activation, are the next candidates, to be
adopted only if measurements show that they pay.

## What RFC 0020 established

Record execution is correct: the whole semantic corpus passes prepared,
multi-shot resumption survives collection after every transition and a tape
round trip, and the base library and compiler run prepared.

It also does less work. On TAK it takes 43% fewer evaluator transitions, and
with lexical addresses it searches no environment by name. Where preparation
removes real dynamic work, wide calls and deep lexical lookups, programs
already run up to twice as fast.

But whole programs break even or lose up to 20%, for two measured reasons:

- **Decoding.** Each transition recognizes a generic `DEFSTRUCT` record by its
  descriptor, finds slot positions, and reads several records per operand. A
  prepared transition costs about 38 ns against 23 ns for a source one.
- **Retention.** Prepared code keeps its whole IR graph alive: bindings,
  owners, parameter records, source scopes. The benchmark machine's heap is
  485 KiB with the library in source form, 693 KiB with the compiler loaded,
  and 1324 KiB with the library prepared. The collector copies all live data
  at every collection, so collection time on TAK doubled.

## Requirements

Lowered code must satisfy two criteria, which together force it to be a
self-contained execution object:

1. **No IR decoding.** Execution never recognizes or decodes an IR record. If
   a reference was lowered, the executor already knows it is performing a
   lexical load.
2. **No IR retention.** Executable code does not keep its IR graph alive.
   Analysis objects die after lowering unless someone retains them. A cache
   attached to the records would meet the first criterion and fail this one.

RFC 0020's semantic commitments and liveness contract carry over unchanged.
In particular:

- All guest control stays in heap frames. Nothing about a suspended
  computation lives in native state.
- A captured continuation can be resumed any number of times. Each resumption
  owns its progress; all share lexical locations.
- The callee of a call is resolved before its arguments and kept across
  suspension. Special operators are fixed and macros are already expanded.
- Environments keep their present representation, a chain of name/value
  vectors, so `ENV`, source escapes, and mixed source/lowered calls keep
  working. Flat closures and unboxed locals remain later projects.

## What a suspended frame needs

The record executor has five kinds of frame. This is everything each one needs
in order to resume:

| Frame is waiting for | It needs |
| --- | --- |
| A branch test | The two continuations to choose between |
| An assignment's value | The target: a lexical address, or a symbol |
| A form in a sequence | The position in the sequence |
| A `LET` initializer | The position, the values so far, the scope's names, the body |
| A call argument | The position, the resolved callee, the values so far |

Every row reduces to the same three things: *where to continue*, *saved
values*, and *the environment*. The existing continuation row already has
exactly those columns:

```text
ktx:  hop   env   fun              acc             arg
            ^     where            saved values    position
            environment
```

Today `fun` holds an IR node and `arg` an index into it. The frame schema does
not need to change for lowered code; only what `fun` points to does. Capture,
the frozen-frame watermark, the rule that a write to a frozen frame first
copies its saved-value vector, collection, and the tape encoding of frames all
carry over.

## Continuation points

A *continuation point* is what `fun` and `arg` name together: a place in code
where execution resumes with a value. Two shapes are possible.

**A. Compact nodes.** Each operation is one small word vector:

```text
[operation, operand, operand, ...]
```

`operation` is a fixnum. Operands are fixnums (addresses, counts), constants,
symbols, or other nodes. A continuation point is `(node, position)`, as today,
and the run's expression register holds a node, as today. The executor
dispatches on the first word instead of decoding a descriptor.

The implementation uses the existing **record** word pool: its type word is
a versioned fixnum opcode, not an IR descriptor. Ordinary vectors retain their
self-evaluating data semantics. Code version 1 encodes the version above an
eight-bit operation index; `src/wisp/code.hpp` owns the operation/operand schema,
which also produces `CODE-OPERATIONS`, runtime checks, and the tape manifest.
Portable tapes are now version 5 and reject older tapes or mismatching code
layouts. No heap tag, run column, or continuation column changed.

**B. Flat code.** Each function is one instruction vector plus a constants
vector, and a continuation point is `(code object, PC)`. Instructions name
their continuations by offset.

Shape A is the smaller step. It changes the representation of code and nothing
about control: the transitions of the record executor map one to one onto
node operations, and every continuation point is a single heap word plus an
index, so no register or frame column needs a new meaning.

Shape B has real advantages. One vector per function is cheaper for a copying
collector than many small rows, it has better locality, and it is the natural
base for later packing. It also has a cost that shape A avoids: the run must
name the current point between steps without native state, and the run row
has an expression register but no program counter. Either the run schema gains
a column, which changes the tape version, or the active activation's frame
always stays on top and holds the PC, which commits to one frame per
activation (below). Saved PCs must also be validated as genuine resume points
of that exact code object when a tape is restored.

Start with A, measure, and move to B if code size or dispatch cost justifies
it. Nothing in A's operations or frames blocks that move: B is a different
encoding of the same operations.

## Node operations

These are the operations the executor already performs, written as nodes. The
list is an observation, not a frozen instruction set; encoding and any
packing are decided by measurement.

| Operation | Operands | Frame while waiting |
| --- | --- | --- |
| constant | value | none |
| lexical load | depth, index | none |
| global load | symbol | none |
| function cell | symbol | none |
| lexical store | depth, index, value node | node |
| global store | symbol, value node | node |
| branch | test, consequent, alternative | node |
| sequence | forms | node, position |
| let | scope names, initializers, body | node, position, values |
| call | symbol, arguments | node, position, callee and values |
| closure | code | none |
| source | form | none |

Three points differ from the record IR.

*Bindings are gone.* A reference becomes a lexical load with its address. Only
the names of each scope survive, as one constant vector per `LET` and the
parameter list per function, because the environment stores names and
reflection and source escapes read them.

*Lexical loads without an address do not exist.* Analysis gives no address to
a binding that has no runtime scope behind it; lowering turns such a reference
into a global load, which is what name lookup would do.

*A function's code is a small object, not a node.* It holds the parameter
list, the body node, the name, and the source body that `CODE` reports. A
closure's code slot holds this object, as it holds the `ir-function` today.
The source snapshot is source, not IR, and the function already retains it in
source mode.

Operands that are constants, lexical loads, or function cells are evaluated in
place by the operation that uses them, as record execution does now, so
`(- x 1)` remains one transition with no frame.

## Who defines the layout

RFC 0020 asked that each operation's metadata be defined once. For lowered
code the definition belongs to C++, because the executor must not learn
layouts from guest-declared structs; that is the decoding this RFC removes.

Declare each operation once in C++ with its name and operand kinds, in the
project's schema style, and derive the dispatch table, the validity checks,
and a description exposed to the guest. The lowering pass is written in Wisp
and builds nodes from that description; `(code-operations)` or a similar
primitive returns it. No operation numbers are written twice. Operation
identities are part of the tape format and need a version, like builtin names.

## Trust and checking

Code records are valid by construction. `MAKE-CODE` is their only
constructor: it checks the opcode, the arity, and each operand's kind
against the native schema, and a NODE operand must itself be a code record.
`RECORD` refuses a code opcode (`INVALID-CODE`), `RECORD-SET!` refuses a
code record (`IMMUTABLE-CODE`), and tape encoding and decoding apply the
same check to every code record. By induction every code record has a
valid shape, so dispatch reads only the opcode and trusts the operands.

NODES and NAMES operands are ordinary vectors, which a guest can still
mutate. Executors check their elements when they use them: an element
entered or evaluated in place must be a code record, and a LET name must be
a symbol. A lexical address must still fit the environment at run time,
with lookup by name unavailable as a fallback because the name is gone. A
failed check signals a condition.

`ir-check` remains the gate before lowering: only a checked graph is lowered,
so well-formedness errors are reported against the IR, where they are
readable. The construction and runtime checks exist so that guest code or
a malformed tape cannot make native dispatch read out of bounds, not to
report compiler bugs.

Immutability needed no new heap type and no tape version: a code record is
still a record whose type word is a code opcode, and the tape layout is
unchanged. Earlier tapes whose code records have valid shapes still load.

## Frame granularity

Record execution keeps one frame per pending operation. TAK pushes about two
frames and allocates about six vector words per function call. There is an
alternative.

**Per operation**, as now. Frames and saved-value vectors are small. After a
capture, the first write to a frozen frame copies one small vector. Nested
calls push a frame each.

**Per activation.** One frame per function call, with one vector of
temporaries for the whole body and a program counter. There are fewer pushes
and no allocation per call site. But the first write after a capture copies
the whole activation's temporaries, the set of live temporaries must be
defined at every resume point so tapes can be validated and frames inspected,
and tail calls must release or reuse the activation without disturbing frozen
snapshots.

A conventional VM chooses the second without discussion. For Wisp it is not
obvious, because multi-shot copying and inspectable progress are what this
design exists to preserve, and the copy cost moves from "one call's arguments"
to "one function's temporaries".

Keep per-operation frames for shape A, where they are free. Decide the
question with numbers before designing shape B, since B's program counter
interacts with it. The profile counters already report frame pushes and vector
words; add a counter for words copied when a frozen frame is written, and run
the effect and router benchmarks, which capture and resume, alongside TAK.

## Retention and code size

Lowering removes bindings, owners, parameter records, and source scopes. What
remains live per function is its nodes, its scope-name vectors, its constants,
and its source snapshot.

The collector copies everything live at every collection. That includes the
library's source conses today, in every mode, so code size is a cost in source
mode as well and lowering competes with it directly. Report live heap size for
the library in source, record, and lowered form, and collection time on the
benchmarks, as primary results.

Two further options follow from the same observation and are out of scope
here: dropping a function's source snapshot when nobody needs `CODE`, and
giving long-lived code a space the collector does not copy. Record the numbers
that would justify them.

## Installing and inspecting

The implementation adds the opt-in `lower-function!`: analyze, check, lower, install the code
object, and drop the graph. `lower-package!` does the same for a package;
`lowered-eval` lowers one source form. `analyze` and `ir-show` are unchanged,
and a caller that wants the IR keeps it by holding the result of `analyze`.

Lowered code needs its own readable view, since the IR is no longer there to
show. Provide `code-show`, the counterpart of `ir-show`, printing operations
by name with their addresses and constants. `KTX-FUN` on a lowered frame
returns a node; give frames a documented view of operation, position, callee,
and completed values, as RFC 0020 requires of prepared frames.

`code-show` returns a named operation tree with explicit lexical addresses.
`code-frame` returns `(operation-view position callee completed-values)` for
a lowered frame, or NIL otherwise. Positions are zero-based. The raw node and
saved progress remain available through the existing KTX accessors.

`CODE`, `SET-CODE!`, and the liveness contract behave as they do for record
execution.

## Stages and acceptance

### 1. Lower to compact nodes — implemented

Declare the operations in C++ and expose their description. Write the lowering
pass in Wisp over checked IR. Execute nodes in the evaluator, with the
record executor left in place for comparison (since retired, stage 4).

Acceptance: the semantic corpus passes in a third, lowered mode. The
suspended-argument example resumes twice from each of two restored tapes with
a collection after every transition. No lowered transition calls the record
decoder; a test that deletes the IR structs' descriptors after lowering still
runs the corpus.

### 2. Drop the graph — implemented with opt-in installation

`lower-function!` lowers and releases the IR; `prepare-function!` remained the
record reference until stage 4. The base library and compiler run lowered, including a
second pass lowering the compiler with the lowered compiler itself.

Acceptance: `wisp-bench` gains a lowered mode. Report, per benchmark and
against source and record modes: time, evaluator transitions, frame pushes,
vector words, live heap, and collection time. The expectation to test is that
lowered transitions cost about what source transitions cost while keeping
record execution's 43% reduction in their number.

### 3. Decide the next representation by measurement

With those numbers, decide whether flat code, per-activation frames, an
immutable code type, or none of them is worth building, and write that
decision into this RFC. A change that needs a new heap type or run column
updates the tape version and its validation in the same patch.

### 4. Retire record execution — implemented

Lowered execution passes everything record execution did, and the record
executor is removed: `prepared-eval`, `prepare-function!`,
`prepare-package!`, the descriptor layout cache, and the `prepared`
benchmark modes. The IR, analyzer, and checker stay. Source interpretation
is the semantic reference; the corpus runs every case in source and lowered
modes. Evaluating a semantic record now signals `invalid-expression`.

Keeping record execution meant keeping a native decoder for guest-defined
struct layouts: it found slot positions by name in each descriptor and
cached them in a table invalidated at every collection, since collection
moves descriptors. Nothing else needs that machinery.

## First compact-node measurements and decision

The [complete results and raw data](https://github.com/mbrock/nxtui/blob/main/bench/wisp/LOWERED.md)
record 500 checked timing samples (five samples, 20 cases, five modes) and 100
separate diagnostic records at
[626484a](https://github.com/mbrock/nxtui/commit/626484af31129089d838fd19ee992faec193ea97).
This is Clang 23.1 on Linux x86-64 in an orb, serial and CPU-pinned, not the
earlier macOS measurement. Before timing, collect setup garbage and give each
mode the same relative allocation headroom; old heap-start numbers are not a
comparable retained-code baseline.

The fully lowered fixture machine retains 21.8% less than the fully prepared
one (256 versus 327 KiB in the fixed call fixture), and fully lowered medians
beat fully prepared medians in all 20 cases. Source remains smaller, because
it does not load the compiler. Against source, wide calls improve 1.63×,
shallow effects 1.46×, and the guest routers 1.12×/1.20×. But whole-library
lowering is not generally faster yet: TAK is 7.4% slower, DERIV/division about
9–10% slower, and backquote 19.5% slower. Benchmark-only lowering is a little
faster on TAK (53.7 versus 56.2 ms); the library still has a material cost.

Lowering preserves record execution's transition and frame-push counts
exactly. TAK still takes 826,921 transitions, pushes 254,437 frames, and
allocates about 764,000 word-pool words. Its average uninstrumented time per
diagnostic-counted transition is about 65 ns lowered, 73 ns lowered-library,
and 38 ns source. These are different mixes of work, not isolated dispatch
costs, so the expectation of source-like average lowered transition cost is
not met and does not by itself identify opcode checks as the remaining cause.

The new progress-copy counter also includes copy-on-write after collection,
which freezes all surviving frames. Differences in library-mode payload-word
counts equal differences in progress cloning exactly. Smaller retained code
changes GC boundaries: TAK's instrumented collection time falls, but the
deep-effect diagnostic collects more often and spends longer collecting.
Smaller code is a win, not a guarantee of uniformly cheaper GC.

**Keep shape A and per-operation frames for now, keep record execution as
the reference, and keep installation opt-in.** The next experiment is native
profiling of call dispatch, argument binding, and frame/vector allocation,
with effects alongside recursive programs. The high remaining allocation
counts make that a candidate, not proof of its CPU share. These measurements
do not yet justify activation-wide copy costs, a PC/run-schema change, an
immutable code type, or sacrificing source inspection.

## Native profile

A `perf` profile of release builds (Clang 23.1, frame pointers, pinned to
one CPU) after the measurements above answers the question they left open.
This was a different, faster machine: TAK takes about 33 ms in source mode.

The interpreter is bound by instruction count, not memory or branches: about
3.6 instructions per cycle with few branch misses. Subtracting setup, one TAK
run costs:

| Mode | Transitions | Instructions/transition | Cycles/transition |
| --- | ---: | ---: | ---: |
| Source | 1.46 M | 366 | 100 |
| Lowered library | 0.83 M | 698 | 177 |

Both take about 146.5 M cycles per run. Lowering removes 43% of transitions,
and each remaining one costs almost twice as much.

Most of the difference is the per-node check. `lowered_operation` validates
every operand of a node, including whether each child node is code, every
time the node is dispatched: from `once`, from `immediate` for each argument,
and from `proceed_lowered`. It was about 20% of lowered self time. An
unrolled, schema-derived check that left children to be checked on entry
gained only about 1%, because the call itself, not the loop, is the cost.
Checking only the opcode, which is unsafe and was measured only as a bound,
made lowered-library TAK 0.79× source, DIVITER 0.84×, and backquote 0.88×,
about 20% faster than checked lowered code.

The checks cannot simply go: code nodes are ordinary records, so `RECORD`
can fabricate them and `RECORD-SET!` can change them after any check, and
release builds do not bounds-check heap accessors. Hence the decision below.

`evaluator::step` is also 19–32% of self time in both modes: each transition
reads and writes the six-column run row, checks `status` several times, and
enters a `try`. Separately, the Nix development shell's hardening flags and
semantic interposition in `libnxt-core.so` cost about 10% in both modes
equally.

**Make code nodes valid by construction.** Guests may not create or mutate
records with a code opcode through `RECORD` or `RECORD-SET!`; a native
constructor checks shape once, tape decoding checks restored code, and
dispatch reads only the opcode. This is now implemented (see Trust and
checking).

Against the preceding commit, with the same hardened release build and
five interleaved samples per case, lowered-library medians improved 13–25%
on every benchmark measured, and lowered TAK fell from 739 to 585
instructions per transition. Fully lowered execution is now faster than
source everywhere:

| Case | Source | Lowered library | Ratio |
| --- | ---: | ---: | ---: |
| TAK | 32.84 ms | 24.43 ms | 0.744 |
| DERIV | 22.4 µs | 16.6 µs | 0.740 |
| DIVITER | 276 µs | 219 µs | 0.792 |
| DIVREC | 258 µs | 196 µs | 0.760 |
| Standard-library lists | 430 µs | 296 µs | 0.688 |
| Backquote | 109 µs | 88.9 µs | 0.814 |
| Router hit | 74.8 µs | 40.1 µs | 0.536 |
| Router miss | 46.4 µs | 24.8 µs | 0.536 |
| call-1 | 0.320 µs | 0.183 µs | 0.572 |
| call-16 | 0.960 µs | 0.413 µs | 0.430 |
| lookup-inner-8 | 1.361 µs | 0.954 µs | 0.701 |
| effect-shallow | 2.87 µs | 1.42 µs | 0.493 |
| effect-deep | 42.2 µs | 29.4 µs | 0.695 |

Times are per logical run (per iteration for the micro cases) on the faster
profiling machine, with this comparison's own iteration counts, so they are
not directly comparable with the sweep in `LOWERED.md`.

## Open questions

- Whether one frame per pending operation survives measurement, or
  per-activation frames justify their copying and validation costs.
- Whether shape B's gains in code size and collection time justify a program
  counter in the run and resume-point validation in tapes.
- How much of the source snapshot must be retained, and by whom.
