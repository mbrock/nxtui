# RFC 0020: Wisp Semantic Code and Bytecode {#rfc_wisp_semantic_code}

Status: proposed. This document specifies an implementation direction; the
compiler, prepared executor, and bytecode VM described here do not exist yet.

## Proposal

Build a compiler in Wisp that turns source into an explicit, inspectable
semantic representation made from ordinary `defstruct` records. Execute that
representation within the existing C++ heap machine before lowering it to a
compact instruction stream. Keep source interpretation available throughout.

The central product is a **code object**: a reusable description of a
computation whose bindings, evaluation order, control flow, and suspension
requirements have been understood. Bytecode is a later executable encoding of
that description. A closure combines code with a captured environment; an
activation records progress through one execution of that code.

The implementation should make unfinished computations easier to inspect while
reducing the work spent rediscovering their structure. Its first decisive
example is a prepared call suspended halfway through argument evaluation,
saved to tape, restored, and resumed twice with correctly shared lexical
mutation. Performance work follows that semantic foundation.

Wisp is a live language, and the compiler must keep it one, but liveness
needs a contract that a compiler can honor. For redefinition this proposal
takes Common Lisp as its reference rather than the source interpreter's habit
of consulting every function cell at every step: special operators are fixed,
macros expand when code is prepared, and calls to global functions stay late
bound. Wisp's heap-resident, multi-shot, tapeable control has no Common Lisp
counterpart. The compiler preserves it exactly, and most of the care in this
proposal goes there.

The recommended sequence is:

```text
source forms
    → macroexpansion driven by the analyzer, with explicit source escapes
    → semantic records with binding identities
    → record execution on the existing heap control machine
    → lowered operations with storage and continuation layouts
    → portable bytecode on the same control machine
```

The initial work does not require a native compiler, a new scheduler, a raw
word-array heap type, or packed stack segments. Each stage must be useful and
reviewable on its own.

## Current foundations

The current implementation, read at `2b6f13f`, already supplies most of the
runtime substrate. @ref rfc_portable_wisp "RFC 0018" records the port and its
deliberate differences from Zig; the @ref wisp "Wisp guide" describes the
current public language and host interface.

| Foundation | Actual implementation | Consequence for this proposal |
| --- | --- | --- |
| Callable closures | `fun` stores environment, parameters, body, name, and call count | Keep callable identity separate from reusable executable code |
| Guest compiler foothold | `compile!` macroexpands a function body and replaces it with `set-code!`; `defun` and `fn` also expand bodies | Extend a compiler written in Wisp rather than moving analysis into C++ |
| Nominal structs | `defstruct` creates a descriptor, constructor, predicate, accessors, and setters | Use existing records for compiler vocabulary |
| Explicit evaluations | `run` stores expression, value, error, environment, `way`, and `meta` | Prepared execution can remain steppable and heap resident |
| Segmented control | `way` contains ordinary frames; `meta` links prompt, binding, and resume boundaries | Reuse the existing segmentation and composition rules |
| Multi-shot snapshots | A KTX allocation watermark freezes frames; writes copy frozen progress | Extend the ownership rule to prepared activation payloads |
| Portable images | Tape stores guest objects, sharing, and suspended computations | Make semantic code and activations portable from their first executable stage |
| Host effects | Guest requests retain success/error continuations; NXT owns native work | Preserve the current effect bridge across execution representations |

The relevant sources are
[object schemas](https://github.com/mbrock/nxtui/blob/2b6f13f/src/wisp/vat.hpp),
[the evaluator](https://github.com/mbrock/nxtui/blob/2b6f13f/src/wisp/eval.cpp),
[the base library](https://github.com/mbrock/nxtui/blob/2b6f13f/src/wisp/base.wisp),
[continuation copying and GC](https://github.com/mbrock/nxtui/blob/2b6f13f/src/wisp/heap.cpp),
[tapes](https://github.com/mbrock/nxtui/blob/2b6f13f/src/wisp/tape.cpp), and
[the guest host adapter](https://github.com/mbrock/nxtui/blob/2b6f13f/src/wisp/host.wisp).

The existing stack is already segmented in its control structure. Inside a
segment, frames are linked heap rows. Packing several activations into one
contiguous allocation is a separate optimization, with additional copying and
ownership decisions.

## Common Lisp as the reference for liveness

The source interpreter answers every redefinition question by accident. It
looks up `IF`, `LET`, and every named operator in its function cell each time
it reaches a form, so any redefinition, including of the control structures
themselves, takes effect on the next step. Carrying that into compiled code
would put a guard in front of every special form and keep a source fallback
at every call site. Nobody relies on that property, and keeping it would make
the compiler both slower and harder to reason about.

Common Lisp is the better reference. It is one of the most thoroughly live
systems in practical use, and its liveness was settled by implementers and
users more than derived from a calculus. The standard states the goal
directly: its
[semantic constraints](https://www.lispworks.com/documentation/HyperSpec/Body/03_bbc.htm)
exist "to minimize the observable differences between compiled and
interpreted programs", not to erase them. The working rules are few:

- Special operators are fixed. Redefining a symbol of the `COMMON-LISP`
  package is
  [undefined](https://www.lispworks.com/documentation/HyperSpec/Body/11_abab.htm),
  and implementations such as SBCL lock the package. The compiler may treat
  `IF` as `IF`, and may open-code standard functions.
- Macros are expanded when code is compiled. A list form whose head names
  neither a special operator nor a macro known at compile time is a function
  call, and an operator undefined at compile time is not an error. Redefining
  a macro affects code compiled afterward; existing compiled callers keep their
  expansion until they are compiled again.
- Calls to global functions go through the function definition at run time, so
  redefining a function updates existing callers. Inline declarations and the
  assumption that a file's own functions do not change underneath it are the
  explicit exceptions.
- Redefining a structure incompatibly is undefined. Where redefinition pays
  for real engineering it gets some:
  [CLOS updates the instances of a redefined class](https://www.lispworks.com/documentation/HyperSpec/Body/f_upda_1.htm),
  and the metaobject protocol lets dependents register for changes.

Few experienced users could recite these rules. They redefine something,
observe whether callers changed, and compile again when they did not. The rules
make that loop predictable while leaving the compiler room to work, and that
is the property Wisp wants.

Wisp is not a Common Lisp implementation and does not seek compatibility with
it. It adopts the same shape of contract for prepared code:

| Operation | Effect on prepared code |
| --- | --- |
| Redefine a core special operator: `QUOTE`, `FUNCTION`, `%FN`, `%MACRO-FN`, `IF`, `DO`, `LET`, `%SET!` | Unsupported. Prepared code keeps the built-in meaning |
| Redefine a macro, including `FN`, `SET!`, and `DEFUN` | Affects code prepared afterward; prepare existing callers again to update them |
| Redefine a global function with `DEFUN` or `SET-SYMBOL-FUNCTION!` | Takes effect at the next callee resolution in every caller |
| A prepared call site finds a macro or special operator in the function cell | Signals a condition at callee resolution, before any argument runs |
| Redefine a primitive in the compiler's declared open-coded set | Unsupported, like the `COMMON-LISP` package |
| Redefine a struct with the same slots | Preserves descriptor identity, as today |
| Redefine a struct with different slots | Creates a new descriptor; existing instances and code keep the old one |
| Edit a function's source conses | No effect on installed code until it is prepared again |

The core special operators are the builtins that receive their arguments
unevaluated in the evaluator's builtin table. `FN`, `SET!`, `DEFUN`, and the
rest of the surface syntax are macros over them, so they follow the macro rule.
The base library already relies on that rule while it bootstraps: it redefines
`FN` and `DEFUN` as it loads, and code prepared later sees the later
definitions. Source interpretation keeps its present behavior; the table only
states what prepared code promises.

Wisp goes beyond Common Lisp in its control. Common Lisp has no first-class
continuations, and SBCL keeps compiled activations on the native stack. Wisp
keeps every activation in the heap, resumes continuations any number of times,
steps runs under a budget, and saves suspended computations to tape. The
compiler must preserve those properties exactly, including in the middle of a
prepared call. Redefinition is where this proposal borrows Common Lisp's
pragmatism; control is where it spends its rigor.

## Semantic commitments

Prepared execution must preserve evaluation results, observable mutation,
condition delivery, dynamic scope, and continuation composition, within the
liveness contract above. An explicitly compiled definition also needs a documented
policy for its source and inspection interfaces. Those policies cannot be
inferred from a successful arithmetic benchmark.

1. Evaluate ordinary arguments left to right. Resolve a named callee before
   evaluating its arguments, as `application()` does today.
2. Keep value and function namespaces distinct. A value lookup searches lexical
   scope first, then consults the current dynamic declaration and bindings or
   the symbol's value cell. Explicit lexical binders remain lexical when a
   symbol's dynamic declaration changes.
3. Preserve shared mutable lexical locations across closures and repeated
   continuation invocation. Preserve the snapshot behavior of dynamic bindings.
4. Snapshot execution progress separately from lexical storage. Capturing a
   continuation must not let one resumption overwrite another's pending work.
5. Keep prompts, bindings, shallow capture, deep guest handlers, composition,
   errors, and host requests on the existing control path.
6. Preserve proper tail behavior. In particular, tail calls and tail resumes
   must not accumulate redundant return frames or boundaries.
7. Preserve `EVAL`'s public scope rule: it excludes implicit caller locals while
   retaining dynamic/effect context. Internal evaluation of a macro expansion
   retains the call-site lexical environment.
8. Retain inspectable, tapeable guest control at every executor safepoint. Native
   pointers, host closures, sockets, and C++ coroutine frames are not saved
   guest execution state.

The C++ source evaluator is the immediate executable oracle. Use the Zig
reference for shared language behavior and retain the documented C++
differences. New compiled-only behavior must be identified explicitly rather
than presented as a property of the reference interpreter.

## Semantic objects in Wisp

Start the compiler library in a separate package, provisionally
`WISP-COMPILER`. The following definitions sketch responsibilities and are not
a frozen API or a complete grammar:

```lisp
(defstruct ir-binding name owner)
(defstruct ir-reference binding)
(defstruct ir-lookup symbol)
(defstruct ir-function-reference symbol)
(defstruct ir-constant value)
(defstruct ir-assignment target value)
(defstruct ir-call callee arguments)
(defstruct ir-branch test consequent alternative)
(defstruct ir-sequence forms)
(defstruct ir-let bindings initializers body)
(defstruct ir-function name parameters bindings body)
(defstruct ir-closure function)
(defstruct ir-source form)
```

An `ir-binding` identifies a lexical binder. It is a compiler object, not an
instance of the mutable runtime location created when that binder executes.
An `ir-reference` points to that binder. `ir-lookup` retains runtime lookup
where lexical resolution cannot safely determine the target. A function
reference describes reading a symbol's function cell, separately from either
kind of value reference.

For a known lexical `x`, the body `(if x (foo x) 17)` can be represented as:

```lisp
(let ((x (make-ir-binding 'x owner)))
  (make-ir-branch
    (make-ir-reference x)
    (make-ir-call
      (make-ir-function-reference 'foo)
      (vector (make-ir-reference x)))
    (make-ir-constant 17)))
```

Here `owner` is the enclosing analyzed function or scope. Both references use
the same binding object. A shadowing declaration creates another object.
Capture analysis traverses executable child/reference edges and compares
lexical owners; it must not treat arbitrary heap reachability through owner
backlinks, metadata, or constants as a use of a variable. Transitive captures
through intervening functions need explicit tests.

Keep scope, binding identity, and parameter syntax until lowering has enough
information to choose storage. Parameters include the existing required,
optional, and rest/body behavior; an arity count alone is insufficient.
`LET` initializers run in the caller's environment and its bindings become
visible together. Nested scope ownership must reflect that rule. Preserve the
interpreter's lookup order when a scope contains duplicate names; do not
silently impose a uniqueness rule or change which occurrence is referenced.

`ir-closure` evaluates to a runtime closure sharing the analyzed function's
code and capturing the current lexical environment. Repeated evaluation of
one function literal can therefore create different closures over one code
object.

### Analysis and publication

Build records mutably while constructing a compilation. Keep derived facts in
analysis records or identity-keyed tables belonging to that compilation:
references, assignments, captures, liveness, and proposed storage. Lowering can
produce a record such as `(lowered-binding source-binding storage slot)`.
Start with simple lists/vectors for these tables; introduce indexing when its
cost matters.

Treat published code graphs as stable. Optimizing or recompiling produces a
new graph; an activation keeps the graph under which it started. Initially
this is an API convention, not an existing heap freeze feature. Ordinary
`record-set!` does not enforce immutability. Direct low-level mutation of a
published executable graph is outside the proposed compiled-code contract;
the executor still checks its input shapes and must not treat mutable guest
data as unchecked native pointers. A later verifier cache requires either
enforced freezing or an explicit invalidation mechanism.

Descriptor identity separates node kinds, but it is not a proof that every
field is well formed. Verify node types, binding ownership, reference scope,
parameter structure, and legal graph edges. Code/control graphs have defined
cycle rules distinct from arbitrary cyclic literal data.

Same-slot `defstruct` redefinition already preserves descriptor identity.
Changing a compiler struct's slots creates a different descriptor; it does
not migrate old code graphs. Give persistent compiler formats an explicit
version and reject or deliberately migrate incompatible graphs.

## Compilation and a live language

Introduce preparation explicitly before changing defaults. Provisional
operations are:

```text
prepare-function(function)            → code object and diagnostics
install-function-code!(function, code) → callable function
function-executable(function)         → installed code or NIL
```

Keep the current `compile!` macroexpansion behavior during the first stages.
Add a separate opt-in operation for installing prepared code. Only reconsider
the old name and bootstrap policy after mixed execution, reflection, and tape
tests pass.

The analyzer expands macros as it reaches each form position, as a Common
Lisp compiler does, rather than trusting a separate expansion pass over the
body. Because the special operators are fixed, it knows every form position:
a list whose head names a macro at preparation time is expanded and analyzed
again, a core special operator is analyzed by its own rule, and any other named
head is a function call, whether or not it has a definition yet. Preparation
therefore executes macros; it is not a pure parsing operation. Keep its errors
and effects in the guest machine.

`ir-source` remains for forms the analyzer does not support yet. It is an
explicit escape to ordinary source evaluation in the same run and call-site
environment, a scaffold for growing the compiler incrementally rather than a
semantic fallback. It must not silently use public `EVAL`, which has different
lexical scope.

### Definition snapshots

Recommend the following initial, opt-in contract:

- A prepared definition snapshots the analyzed syntax and parameter structure.
  Mutating its original source conses does not rewrite installed executable
  code. This is a deliberate difference from source interpretation.
- Literal objects retain their identity and ordinary mutation semantics.
  Separating syntax from literals must follow form semantics; blindly
  deep-copying all reachable conses would change quoted data and sharing.
- `CODE` continues to expose the source body. A new executable accessor and
  inspector distinguish it from the source snapshot associated with a compiled
  version. Editing source and explicitly preparing it again creates a new
  version.
- `SET-CODE!` clears the function's installed executable for future calls.
  Existing prepared activations keep their previous code object. Their state
  is never relocated into another version during execution.
- Updating a symbol's function cell remains effective at subsequent callee
  resolution. A call that already resolved its function keeps that result
  while evaluating its arguments, including across suspension.

These rules make compilation reviewable without requiring a mutation journal
for every source cons. Like Common Lisp's `COMPILE`, preparation is an explicit
act. Transparent automatic compilation would require a stronger
compatibility/invalidation design and is deferred.

### Late binding at call sites

A prepared named call reads its callee's function cell when the call begins,
before evaluating any argument, and keeps that value for the rest of the call,
including across suspension and repeated resumption. It classifies the value
at that point. A function, function jet, or continuation is invoked with the
evaluated arguments. A macro, special operator, or invalid value signals a
condition before any argument runs; adopting a new macro means preparing the
caller again. No call site keeps source for an alternative path.

Special forms compile to their built-in meaning without a guard. That is the
liveness contract's main simplification: the analyzer and later lowering stages
may treat `IF`, `LET`, and the other core operators as fixed control
structure.

Primitive specialization later needs only the expected operand types, for
primitives in an explicitly declared open-coded set; the contract fixes the
callee. Overflow and type failures use the existing condition path. Functions
outside that set stay late bound even when an instruction exists that could
implement them. Declare the set when the first specialized instruction lands,
and keep it short.

## Code, closures, and activations

Keep three ownership layers visible:

| Object | Owns | Shares |
| --- | --- | --- |
| Code object | Semantic operations, parameter description, provenance, eventual lowered layout | Literal objects and compiler binding identities |
| Callable closure | Captured environment, name/call accounting, installed code reference | Code with other closures; lexical locations with its environment |
| Activation | Current node/PC, operation phase, resolved callee, temporary results, return destination | Stable code; mutable lexical locations |

Prefer adding an optional executable reference to the existing callable
representation over making every record callable. Today `call()` accepts
functions, macros, jets, and continuations; a `compiled-function` struct alone
would not change dispatch. Keep the existing source body for reflection and
preparing again. Changes to the shared `fun`/`mac` schema, call accounting, and tape
version must be made together.

The first production executor should be a C++ transition engine over the
guest-declared records. Analysis, transformations, compiler metadata, and
inspection helpers belong in Wisp. Native code owns dispatch, safe heap access,
and integration with the existing evaluator. It reads record layouts from
their declared descriptors, following the host's named-slot approach, rather
than duplicating Wisp slot order in hand-maintained C++ tables.

A small Wisp-written evaluator can help explore the IR, but it is not a
prerequisite or the intended performance path. In particular, a recursive
meta-interpreter would expose its own interpreter frames and require an
additional account of guest stepping and introspection.

### Activation state and segmented control

Represent prepared work as an explicit frame variant participating in
`run.way`; continue to use `run.meta` for boundaries. Define its entry, return,
and transition dispatch alongside source frames. The exact allocation of
payload fields belongs to the first executor patch, but the payload needs:

```text
code object
current node or portable PC
operation phase and next argument
resolved callee, when a call is in progress
temporary/argument values
return destination
```

The lexical environment is shared storage. The fields above describe control
progress. Returning from a source call into a prepared frame, or the reverse,
must work within the same run. A host call that evaluates a prepared body to
completion on a hidden C++ stack is not an acceptable implementation.

Extend `writable_frame()` and `copy_continuation_frame()` to copy the entire
mutable activation payload before mutating a frozen activation. Copy its
progress record and mutable temporary/argument vectors; keep code, literal
values, and lexical locations shared. The existing watermark freezes KTX rows;
it does not automatically protect a new record or vector referenced by one.
A shallow frame copy that retains a mutable progress record is insufficient.
GC and restore must establish the same frozen-state invariant as capture.
Persistent payloads remain an alternative to measure after this direct
extension of the existing ownership rule works.

### The defining multi-shot example

Under an enclosing handler for `PAUSE`, evaluate:

```lisp
(let ((x 0))
  (list
    (do (set! x (+ x 1)) x)
    (send! 'pause)
    (do (set! x (+ x 1)) x)))
```

The handler retains the continuation and returns without resuming it. At that
point the `LIST` callee is resolved, the completed argument prefix is `[1]`,
and the shared location for `X` contains `1`. Sequentially resuming that same
continuation with `10`, then `20`, must return `(1 10 2)`, then `(1 20 3)`.
The first argument is not reevaluated. Each resumption owns its subsequent
argument progress; both reach the same lexical location.

Repeat the example with capture during resumption, a dynamic binding around
the capture, nested prompts, GC between every transition, and tape restoration
before the first resumption. Restoring two copies of a tape produces two
independent machines; resuming twice inside one machine shares that machine's
lexical store. The distinction must remain visible in tests and inspection.

## Lexical storage and reflection

Retain the current environment representation initially: a chain of alternating
name/value vectors. Binding identities can first resolve through this store
using the current lookup rules. Semantic analysis does not require immediately
replacing environments with registers or flat closures.

`ENV` and `KTX-ENV` expose actual storage, and existing tests assert environment
identity. Common Lisp offers nothing comparable; its debuggers show the locals
that the compilation policy kept. Wisp keeps the access, with a contract in the
same spirit as redefinition. Assigning a value through an exposed environment
is supported and visible to prepared code. Changing the shape of a prepared
activation's environment, by adding, removing, or reordering names in its
vectors or spine, is unsupported. Stage 0 confirms that no built-in operation
extends a lexical environment in place. Prepared code may then use lexical
addresses computed by analysis without a shape guard.

Begin with the current lookup through the shared store, then introduce those
addresses. Environments supplied from outside to `start()` and runtime-created
syntax keep the existing lookup.

Flat captures, boxing, and unboxed locals come later. Storage analysis must
include closures, continuation capture, reflective environment escape, source
escapes, and mutation through aliases. “Assigned and captured by a nested
function” is not sufficient: a local can remain observable through repeated
continuations or an exposed environment even without such a nested function,
and an `ir-source` escape can name any visible local. A scope that calls `ENV`
or contains a source escape keeps materialized storage. Beyond that, follow a
Common Lisp style debug policy rather than promising that every local is always
inspectable: a high debug setting keeps every local in an inspectable
environment, and lower settings show what the chosen storage retains.

Keep the existing `KTX-*` view unchanged for source frames. Give prepared
frames a documented inspection view with code, position, lexical environment,
callee, and completed arguments. Preserve actual environment identity rather
than synthesizing a detached name/value list. Prepared-frame shape and stepping
positions are explicit compiled-mode differences; do not pretend an optimized
activation reproduces every source-interpreter frame. Before enabling prepared
code by default, audit callers of `KTX-*` and provide the necessary projections
or keep those callers on source execution.

An inspector should follow a reference to its compiler binding, explain the
chosen runtime location, and show that location in a particular activation.
Precise subexpression spans are additional metadata to build; the current
loader's top-level source location does not already provide them.

## Stepping, GC, and the host

Preserve the outer `step(run)` / `advance(run, budget)` interface. Initially one
prepared step performs one record-machine transition; one VM step performs one
defined instruction transition. Source mode retains its present meaning.
Counts need not match across representations, and diagnostics/tests must say
which representation they are measuring.

Zero budget only polls. Exhaustion leaves a resumable run. Every return from
the executor commits all live guest state to rooted heap objects. Collection
continues to occur between evaluator calls, and an explicit GC request makes
`advance` return early. Temporary native views cannot survive heap growth or
collection. Nested `STEP!` retains the existing active-run checks and dispatch
behavior.

Do not initially switch to charging only calls and backedges. That would make
budget behavior another simultaneous change. Neither the existing step budget
nor this proposal is a wall-clock bound; individual operations can scan guest
data. Long-running primitives remain a separate scheduling concern.

Prepared execution uses the same `:host` request and resumption protocol.
Changing executor representation does not implicitly add preemption, guest
workers, or structured concurrency. Current synchronous guest computation can
still occupy the host until return or an explicit await.

This proposal does not change nxtrt deck, exec, pool, or blocking-work semantics.
If a later implementation does, update `nxtrt/runtime.rkt` with that change and
run `make spec` as required by the repository instructions.

## Lowering to bytecode

Lower only after record execution establishes the semantics. Introduce explicit
temporary destinations, argument windows, branch targets, return destinations,
and continuation layouts. Binding identities remain linked to their lowered
storage decisions. A modest ANF-like sequence of operations and blocks is
sufficient initially; a general CPS optimizer is not required.

The first operations should cover constants, value lookup/assignment,
lexical access, function-cell resolution, branches, closure construction,
calls, tail calls, returns, and the source escape. Existing jets implement
control/effect operations through the common runtime. Specialized effect or
arithmetic instructions can follow measured need.

Function resolution precedes argument operations. A lowered ordinary call has
the conceptual order:

```text
resolve and classify callee → saved function
evaluate arguments left to right → argument window
invoke saved function with that window → destination
```

A call window can eliminate repeated argument-list scanning and per-argument
consing where they occur. It does not erase required rest-list construction,
lexical storage, or observable suspended argument progress. Tail reuse must
respect frozen snapshots and dynamic boundaries.

Keep separate contracts for analysis, lowering, encoding, and verification.
The encoder assigns instruction offsets and checks ranges; it does not perform
lexical resolution or invent control semantics. Retain the record executor as
an executable comparison target when the bytecode executor arrives.

### Initial storage choice

Use a `v32` containing valid Wisp fixnums for an initial, unpacked opcode and
operand stream. Keep literal values in a separate traced constants vector.
Choose portable opcode identities and an explicit code-format version; neither
C++ enum order nor the current jet index is a durable identity.

The GC traces every `v32` element as a Wisp word. Arbitrary raw `uint32_t`
instructions are unsafe there. Fixnums have 31 representation bits, with bit
30 serving as the sign bit; their numeric range is `-2^30` through `2^30-1`.
Clearing bit 31 produces a valid fixnum, not necessarily a nonnegative one.

A later 7-bit opcode plus three 8-bit operands fits those 31 representation
bits, but requires signed-aware construction and explicit decoding. Defer that
packing and define wide operands before imposing 8-bit limits on programs.
Opaque bytes are another later option. A new raw-word heap object is not needed
to begin.

Define each native operation's metadata once: encoding, operands, validation,
and display name. Derive verifier/disassembler information using the project's
C++23 schema style, and expose that description to the guest emitter. Avoid
independent numeric opcode tables in C++ and Wisp. Use ordinary portable
dispatch first; threaded/native forms remain rebuildable caches.

## Persistence and verification

Semantic records, descriptors, bindings, code graphs, constants, and activations
must survive GC and tape round trips with sharing and identity intact. Treat
node IDs, portable PCs, and code versions as saved state. Native dispatch
addresses, descriptor lookup accelerators, and future generated machine code
are rebuilt after restore.

Adding a callable field or a new continuation variant changes machine
validation. Update the tape schema/version when that happens; current tapes
use version 4. Define explicit migration or rejection for older versions. Even
a record-only extension requires code-format compatibility checks before it
can execute. Serializable records alone do not establish that compatibility.

Verification covers descriptor versions, operation kinds, operand ranges,
instruction boundaries, branch targets, constants, binding/capture references,
temporary use before initialization, and frame payload shapes. A saved PC must
name an executable boundary in the exact saved code version. Reject malformed
prepared state through the appropriate guest condition or tape error before
native dispatch can dereference it. Restored code remains trusted executable
input, under the existing tape trust policy.

Code mutation cannot bypass these checks through a stale “verified” flag. Keep
runtime checks until publication or versioning makes a cached result valid.

## Implementation stages and acceptance criteria

### 0. Establish the contracts and comparison harness

Catalog current source behavior and add focused tests where the compiler needs
an explicit distinction: callee resolution timing, function and macro
redefinition, parameter handling (including when `&optional` defaults run and
which bindings they see), source mutation, and assignment through exposed
environments. Record the liveness contract, prepared-mode snapshot, and
inspection differences from this RFC. Refresh the benchmark baseline on the actual build
used for the experiment.

Acceptance: a named semantic corpus and a documented mode/behavior matrix. No
runtime default changes and no performance claims from historical timings.

### 1. Analyze into records in Wisp

Add the compiler's record definitions, scope walker, binding resolution,
parameter description, source associations, graph validation, and a readable
inspection helper. Start with constants, references, `IF`, `DO`, `LET`, function
literals, assignment, and named calls, expanding macros as the walker
reaches them. Mark unsupported forms with explicit source escapes.
Keep `compile!` and the source executor unchanged.

Acceptance: shadowing, simultaneous `LET`, nested/transitive captures, optional
and rest parameters, function/value namespaces, expansion at preparation
time, operators undefined at preparation time, and cyclic source
rejection/handling are tested. Analyze a small function, inspect it, GC
it, and save/restore its graph while preserving binding and descriptor identity.

### 2. Execute semantic records in the existing machine

Add the explicit prepared-frame variant, dispatch transitions, code/closure
attachment, return path, source escape, and continuation copy policy. Begin
with current lexical environments and conservative lookup. Support mixed calls
in both directions and route jets through existing semantics. The new frame
variant enters tape validation and the tape version in this stage, not later.

Acceptance: the multi-shot argument example passes in source and record modes,
including GC between steps and tape restoration. Nested prompts, dynamic
binding snapshots, shared lexical mutation, conditions, tail recursion, and
host timer suspension work across mixed frames. Inspect the actual saved
activation and verify its environment identity and frozen argument progress.

### 3. Complete the live-code contract

Implement the opt-in preparation/install operations, source/executable
inspection, version retention for suspended activations, `SET-CODE!`
invalidation, and the condition signaled when a callee has become a macro or
special operator. Specify and test
the prepared `KTX-*` projections. Bootstrap compiler descriptors without
requiring a compiler to load itself.

Acceptance: every row of the liveness table, source edits, recompilation,
redefinition during arguments, macro effects, and interpreted/prepared reentry
have documented, tested outcomes.
The existing source-only suite retains its behavior. Every prepared corpus
case reports whether it executed prepared nodes or used a source escape.

### 4. Lower and execute portable bytecode

Introduce explicit operations, temporary layouts, argument windows, portable
PCs, the fixnum instruction stream, verifier, disassembler, and native dispatch.
Reuse the record executor's store, boundaries, conditions, and activation
ownership rules. Extend tape validation and inspection in the same patches.

Acceptance: the semantic corpus passes in source, record, and bytecode modes
under their declared contracts. Suspend and restore at every reachable
instruction boundary in small programs. Exercise wide operands and malformed
code. Differential runs must demonstrate real VM execution, not universal
fallback to the source evaluator.

### 5. Optimize from measurements

Measure dispatch, allocation, validation, lookup, capture/copying, GC, compile
time, and image size. Candidate changes include direct lexical addresses,
argument-window reuse, safe operation fusion, guarded fixnum arithmetic, and
proven storage simplification. Keep an unoptimized execution mode.

Acceptance: each optimization retains the semantic corpus and reports its
cost/benefit on relevant workloads, including effects and allocation. Mutating
or removing an essential guard must make a corresponding test fail. No fixed
speedup target substitutes for evidence.

### 6. Compile the library and consider defaults

Bootstrap records and the compiler through the source interpreter, then
explicitly prepare selected library functions. Extend coverage until the
compiler can compile its own supported implementation and the resulting boot
tape restores without a native compiler cache. Preserve a source-only boot and
debug path. Add code-version/coverage reporting before changing CLI defaults.

Acceptance: prepared boot, mixed host integration, compiler self-hosting,
reflection, and cross-process restore pass on supported toolchains. Any change
to `compile!`, default compilation, or public stepping documentation is a
separate visible decision. Call-count tiering, native templates, Wasm code
generation, packed segments, and flat closures remain subsequent projects.

## Validation and measurement

Run source, record, and VM comparisons in independent copies of a base image;
multi-shot mutation makes reuse of one shared test world misleading. Compare
results, ordered effects, conditions, and observable store changes. Compare
alias/identity relationships within each machine rather than raw pointer
values across images. Representation-specific step counts and inspection
layouts have their own assertions.

The corpus should include:

- Lexical shadowing, assignment, optional/rest arguments, shared closures,
  function/value separation, and changing dynamic declarations.
- Function redefinition before and during argument evaluation; a function
  cell that becomes a macro or special operator, signaling at callee
  resolution; macro redefinition and preparing again; primitive error and
  overflow paths.
- Captures before/between/after arguments; repeated, nested, shallow, deep,
  and tail resumption; dynamic mutation versus shared lexical mutation.
- Actual `ENV`/`KTX-ENV` identity, assignment through exposed environments,
  public `EVAL`, and call-site macro expansion.
- Mixed source/prepared/VM recursion, conditions and source escapes, GC at each
  transition, pending timers, cancellation/error resumption, and tape forks.
- Code/descriptor/PC corruption, incompatible versions, wide instructions,
  and explicit recompilation while older activations remain suspended.

Use the existing
[Wisp tests](https://github.com/mbrock/nxtui/blob/2b6f13f/test/wisp-eval-test.cpp),
[tape tests](https://github.com/mbrock/nxtui/blob/2b6f13f/test/wisp-tape-test.cpp),
[runtime tests](https://github.com/mbrock/nxtui/blob/2b6f13f/test/wisp-runtime-test.cpp), and
[base-image helper](https://github.com/mbrock/nxtui/blob/2b6f13f/test/wisp-base.hpp).
Add mode coverage to relevant
tests rather than mechanically asserting identical source frame counts in an
optimized executor. Mark slow integration/stress work according to repository
conventions.

The [2026-10-02 benchmark report](https://github.com/mbrock/nxtui/blob/2b6f13f/bench/wisp/RESULTS.md)
is a historical baseline, not proof of the current bottleneck. It reports TAK
at roughly 93 ms for C++ Wisp and 2.2 ms for CPython under its recorded setup,
but its semantic
counters do not establish the fraction of time attributable to lookup or
allocation. Use
[the benchmark harness](https://github.com/mbrock/nxtui/blob/2b6f13f/bench/wisp/README.md)
to measure current revisions and separate preparation cost, steady execution,
GC, capture/resume, tape size/restore time, and end-to-end process cost. Include
argument-width, lexical-depth, recursion, closures, effects, and list workloads.

For implementation patches, run the affected Wisp suites and integration paths;
at integration milestones run the full Meson suite, `nix build`, and
`nix flake check`, including install-consumer coverage. Keep public-header
installation rules in sync when adding C++ files. A documentation-only change
to this RFC requires the documentation build and link checks, not a claim that
the proposed executor has been implemented or tested.

## Implementation references

The
[Common Lisp HyperSpec](https://www.lispworks.com/documentation/HyperSpec/Front/index.htm)
is the reference for the liveness contract, in particular its semantic
constraints on compilation and its constraints on the `COMMON-LISP` package.
[Dybvig's Three Implementation Models for Scheme](https://legacy.cs.indiana.edu/~dyb/papers/3imp.pdf)
is the starting reference for compilation with heap environments and control.
[The Implementation of Lua 5.0](https://www.lua.org/doc/sblp2005.pdf) supplies
concrete register and call-window ideas for the lowering stage.
[CMUCL's compiler architecture](https://cmucl.org/doc/different-compilers.html)
illustrates interpretation and byte/native compilation sharing an intermediate
representation.
[Guile's template JIT](https://www.gnu.org/software/guile/manual/html_node/Just_002dIn_002dTime-Native-Code.html)
is a later reference for making native execution follow an inspectable VM
model. These are implementation precedents; the Wisp contracts above decide
which techniques can transfer.

## First implementation slice

After the contract fixtures in stage 0, the first compiler patch should deliver
stage 1 only: compiler records, an analyzer for a small explicit subset,
binding-identity tests, and an inspectable saved code graph. It should end with
a Wisp programmer holding the analyzed form of `(if x (foo x) 17)` and seeing
both uses of `x` point to the same binding.

The next slice should make that representation execute in the existing run,
then grow immediately toward the suspended-argument example. Delay instruction
packing until we can inspect, save, restore, and resume that computation with
the intended semantics.
