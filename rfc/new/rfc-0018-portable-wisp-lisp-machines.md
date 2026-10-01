# RFC 0018: Portable Wisp Lisp Machines {#rfc_portable_wisp}

Status: new

## Summary

Develop a modern C++ implementation of Wisp with the same Lisp semantics,
integrated with NXT. The central object is a portable Lisp machine: a live
environment that an agent can inspect, extend, suspend, save, restore, and fork.
Its execution state belongs to the Lisp heap, so a saved image can carry an
unfinished computation as well as its data and definitions.

This RFC proposes the C++ port and its NXT integration. Existing behavior,
historical experiments, and proposed extensions are distinguished below. Storage
choices and public APIs remain open until small implementations establish their
behavior.

## Motivation

Wisp already combines live Lisp definitions, explicit evaluator state, delimited
continuations, native and WebAssembly execution, and heap images. The C++
implementation should preserve these properties while making Wisp easy to embed
in NXT applications. NXT supplies scheduling, asynchronous operations,
behavioral coordination, terminal presentation, and model integration.

Agents should be able to work inside their own Lisp environments: define
helpers, inspect objects and suspended computations, change code, and continue.
A machine can be saved as a file, forked into another experiment, or restored in
another compatible host. External resources need a separate rebinding protocol;
a heap image cannot itself preserve a live socket, process, or arbitrary host
object.

A first application is the live voice writing environment discussed alongside
Swash. Tentative recognition, revised recognition, interpretation, editing
suggestions, and committed prose can coexist as distinct hypotheses. Behavioral
rules govern when each hypothesis changes and when an edit is published. Audio
archiving and system integration remain useful adjacent services with their own
lifecycles.

A possible workbench would show the current document, live hypotheses, Lisp
objects, and suspended request cards. A request card would expose the
operation's available continuations or restarts. Users and agents could act on
the same structured descriptions. This is a proposed interface, not a current
implementation.

## Existing foundations

The original [Wisp platform
notes](https://github.com/mbrock/wisp/blob/6c735711ad34688665009fe40cceb4d3a7564271/etc/platform.org#L19)
already connect serializable continuations, suspended processes moving between
nodes, restartable conditions, a dashboard for resolving conditions, and
replicated heap transactions. The present project develops that direction with a
C++ implementation and NXT as an embedding environment.

Wisp's current [deep effect
handler](https://github.com/mbrock/wisp/blob/6c735711ad34688665009fe40cceb4d3a7564271/core/lisp/base.wisp#L470)
receives a request and explicit functions for resuming with a value or raising
an error inside the suspended continuation. Resumption reinstalls the handler.
Standard input and output are [handled
effects](https://github.com/mbrock/wisp/blob/6c735711ad34688665009fe40cceb4d3a7564271/core/lisp/base.wisp#L643);
JavaScript promise completion and rejection also use the handler protocol.

The browser debugger retains a condition, its continuation, and a body for
retry. Its generic actions supply a value, use nil, retry the entire guarded
body, or abort. The richer operation-defined restart protocol in
[CONDITIONS.md](https://github.com/mbrock/wisp/blob/6c735711ad34688665009fe40cceb4d3a7564271/CONDITIONS.md#L765)
is a design proposal. It should be developed as an extension after the port
reproduces existing behavior.

NXT already contains a behavioral coordinator, `game<Event>`, whose
[documentation identifies Swash 2024 as its ancestor](../../docs/rt-game.md).
This provides an existing place to explore temporal rules around guest requests
and application events.

## Proposed integration boundary

Think of the result as a scripting runtime in the same broad sense as Node.js:
a language machine together with an event loop and host facilities. Here NXT
supplies the event loop rather than Wisp growing a second scheduler. One host
turn advances a bounded amount of guest work; an I/O completion makes a rooted
guest continuation runnable again. The language machine can also be stepped
without an event loop, which keeps embedding and semantic tests small.

```text
editor and agents
    structured requests and restart descriptions
        NXT host adapter
            Wisp machine: values, definitions, evaluator, continuations
            NXT runtime: tasks, firms, platform I/O, model calls, presentation
```

The host advances a guest machine in bounded steps until it completes, reaches a
scheduling boundary, or exposes a request. The guest retains its future as heap
data. NXT services the request and delivers a result or condition through a
rooted guest continuation. Host tasks and registrations may need cancellation or
asynchronous teardown before their storage can be reused; restoring an image
recreates those host bindings under an explicit policy.

The first adapter should use existing NXT machinery. It does not require the
unimplemented wish-draining firm or first-order game-card proposals catalogued
in [State of the Frontier](rfc-0017-state-of-the-frontier.md). A durable guest
behavioral coordinator can be investigated separately from the current
coroutine-based `game<Event>`.

## Preserve Wisp semantics

The existing Zig implementation is the behavioral reference. Preserve reader
behavior, value representation, packages and symbol identity, macros, lexical
and dynamic binding, function redefinition, evaluation order, mutation,
condition propagation, prompt delimitation, continuation composition, and
repeated continuation invocation.

Continuation copying needs particular care. Wisp [shares lexical environments
but copies partially filled argument
vectors](https://github.com/mbrock/wisp/blob/6c735711ad34688665009fe40cceb4d3a7564271/core/heap.zig#L571):
the former are shared store, while the latter are mutable control state. A
general environment clone would change this behavior.

Keep guest execution and continuations as explicit heap objects. NXT's C++
coroutine frames can perform host work and drive the machine, but they should
not become the serialized representation of Lisp control state. Host task
ownership, stop tokens, and native environment bindings have their own
semantics.

Language compatibility and compatibility with existing tape bytes are separate
targets. Determine whether the initial port must read and write the current tape
format. Preserve explicit tag and field identities, fixed-width values, and
documented encoding rules rather than deriving a durable format from tuple order
or compiler object layout.

## Heap schemas and column storage

The proposed vat is a tuple of schema-specific tables. Within each table, fields
have a common row domain and columnar storage. Each schema declares its Wisp
tag, ordered fields, stable field identities, and the meaning of each field.
Physical storage can remain 32-bit words even where the API distinguishes Lisp
values, offsets, lengths, counters, and external identifiers.

Moppe's
[Bundle](https://github.com/mbrock/moppe/blob/3000786a292b5f54ba82b69681773919a93a63ec/moppe/spatial/bundle.hh#L89)
is the main inspiration: heterogeneous typed columns over one domain, access by
semantic specification or position, lightweight row views, and compile-time
rejection of repeated specifications. Its domain is finite; a Wisp table's row
domain must grow during allocation and change during collection. Borrow the
abstraction without importing terrain, interpolation, or physical-unit
requirements into the guest heap.

An illustrative declaration, using proposed types, is:

```cpp
using duo = schema<tag::duo,
    field<"car", lisp_value>,
    field<"cdr", lisp_value>>;

using byte_vector = schema<tag::v08,
    field<"idx", byte_offset>,
    field<"len", byte_count>>;

// Other schemas are declared in the same way.
using vat = std::tuple<
    tab<duo>, tab<symbol>, tab<function>, tab<macro>,
    tab<byte_vector>, tab<word_vector>, tab<package>,
    tab<run>, tab<context>, tab<external>>;
```

The tuple assembles tables with different schemas. Inside a table, an
`std::array` of column owners is a simple initial storage strategy because
current fields are uniformly 32 bits. A packed allocation with derived column
spans is another candidate. Keep that choice behind the table interface and
measure it before making performance claims.

The schema should support field access, collector traversal, debugger
presentation, tape encoding, and journal records. Tracing still needs explicit
rules for raw metadata and tag-specific payloads. Moppe also has an [Étalon
experiment](https://github.com/mbrock/moppe/blob/3000786a292b5f54ba82b69681773919a93a63ec/etalon/src/bundle.zig#L41)
that derives a Zig MultiArrayList from a row schema; it provides another useful
reference.

## Moving collection and borrowed views

Port
[Tidy](https://github.com/mbrock/wisp/blob/6c735711ad34688665009fe40cceb4d3a7564271/core/tidy.zig#L40)
faithfully before changing the collector. It creates a destination heap with the
era flipped, copies reachable objects, and scans new rows and word-vector
contents until all scan cursors catch up. The old rows hold forwarding
information: the first two columns temporarily become a forwarding marker and
destination pointer. That collector representation must be supported explicitly
by the table implementation.

There is an important allocation boundary in [field
tracing](https://github.com/mbrock/wisp/blob/6c735711ad34688665009fe40cceb4d3a7564271/core/tidy.zig#L223).
Copying a referent can grow the same destination table that is being scanned.
Read the field value, relocate it, then resolve the destination storage again
before writing. A long-lived C++ reference or span into a column can become
invalid during this operation.

Use lightweight row views containing a table identity and row index, with a
documented lifetime across allocation and collection. Use registered roots or
pins for host values that must survive a collection. Root registration and
release can have RAII wrappers, but guest reachability remains the collector's
responsibility.

Current Tidy copies live word-vector payloads into a new pool while retaining
the existing byte pool. Browser external objects have separate release behavior.
Keep these policies explicit. Collection order and root ordering matter if a
replica must reproduce the same relocated state. Decide how collection failures
and journal publication are handled before claiming transactional replay.

## NXT Storage Farm and Hub

NXT currently has both `nxtrt::farm` and a vendored `boost::container::hub`.

[Farm](../../src/nxtrt/farm.hpp) manages reusable slots in borrowed storage. The
current `farm<T>` accepts a runtime-sized span of slots plus borrowed storage
for free-index bookkeeping. `farm<T, N>` provides inline index bookkeeping over
a caller-owned `std::array<T, N>`. The current implementation no longer requires
a power-of-two capacity.

Recently returned indices pass through a hot FIFO ring. The remaining free
indices live in `mask`, which refills the ring with the lowest indices first.
The current [mask](../../src/nxtrt/land.hpp) is a 64-ary summary tree: each
level records which child words are nonempty. Inline `mask<N>` and borrowed
runtime-sized `mask<>` share this structure. Allocation returns `hope<T*>`, with
null when all slots are handed out; this is not a promise to wait for a later
release.

Farm suggests useful patterns for bounded host resources: pending-request slots,
staging buffers, and other records with explicit acquisition and return. Its
borrowed storage and compact free-index bookkeeping can inspire heap storage
policies and bounded table segments. Guest allocation still needs Wisp's growth,
reachability, relocation, and image semantics.

NXT's [vendored Hub note](../../vendor/hub/README.nxtui.md) identifies the
upstream library as Joaquin M Lopez Munoz's Hub and explains its use for
stable-address execution records. The kqueue, io_uring, and epoll wands hold
`boost::container::hub<exec>` containers. These are concrete current uses of the
vendored header; no assumption about availability in an installed Boost release
is required.

In the [kqueue wand](../../src/nxtrt/wand/kqueue.hpp), native event registration
refers to a record's address, and public wait tokens encode that address.
[Retirement](../../src/nxtrt/wand/kqueue.hpp) delays erasure until registration
users are gone. This is a useful host-side lifetime model for effects. A
portable guest request should instead carry an image-safe identity that the host
maps to its current execution record.

[RFC 0004](rfc-0004-wand-completion-routing.md) proposes direct task-slot
routing for common one-shot completions, while retaining execution records for
operations that need them. The Wisp adapter should work with either host
realization. Its portable request identity must not depend on Hub remaining the
default container.

The [land layer](../../src/nxtrt/land.hpp) gives storage one home:
`value_storage_ref<T>` borrows, `static_value_storage<T, N>` embeds storage, and
`rack<T>` owns it. `junk<T>` distinguishes uninitialized capacity from live
values. A table can follow this ownership discipline while retaining its own
rules for growth and collection. Raw capacity and initialized rows should remain
distinct.

Other NXT patterns are also relevant: compact [index-and-era task
IDs](../../src/nxtrt/ids.hpp), explicit [storage owners and borrowed
references](../../src/nxtrt/land.hpp), tuple composition, ready-or-pending
`hope<T>`, and the [prepared, parked, settled, retired execution
lifecycle](../../src/nxtrt/exec_lifecycle.hpp). Reuse their discipline where the
semantics fit.

## System requests and operation defined restarts

Proposed protocol: an operation emits a structured request and suspends its
continuation. A host handler, behavioral policy, agent, or user decides how it
proceeds. Ordinary service requests and exceptional conditions use related
control machinery, while retaining their distinct meanings.

The operation should define its meaningful recoveries. A model request might
accept a supplied result, retry with another provider, or cancel its enclosing
operation. A parser might accept a replacement value or skip a record. Each
descriptor should contain a stable identity, human-readable report, argument
descriptions, association with the suspended request, and a bounded invocation
capability.

The Wisp-specific design is to send the selected restart request into the
captured continuation, where a local handler performs the recovery. The
continuation is both a suspended future and an address into its still-live
dynamic context. Exact syntax and handler return behavior remain open questions
in CONDITIONS.md.

The same descriptions should support a browser or terminal card and automatic
agent policies. Fast decision models could choose among well-defined
alternatives; more capable agents could inspect the surrounding environment or
develop new Lisp helpers. Model choice belongs to the host policy and can change
without changing the guest evaluator.

## Images mutation journals and external effects

The intended persistence model is a checkpoint image plus a sequence of changes.
The historical [replication
notes](https://github.com/mbrock/wisp/blob/6c735711ad34688665009fe40cceb4d3a7564271/etc/platform.org#L62)
describe a primary instance performing I/O and broadcasting heap transactions to
inspectable replicas. The old heap emitted allocation and mutation records. The
implementation was removed on April 27, 2022 in
[a155168](https://github.com/mbrock/wisp/commit/a15516852346e6f9bad4e84232270cd736490e67),
with the stated reason that it was unmaintained. The inspected code contained a
writer and a REPL buffer, but did not establish a complete durable replay
implementation.

A new heap journal should cover allocations, field and row updates, payload
changes, relevant root and pin changes, and collection boundaries. Normal
mutation must pass through tracked operations or slot proxies. Mutable column
references require controlled access so writes cannot silently bypass the
journal. Stable schema identities should make records interpretable across the
port.

Use a separate external-effect ledger for requests and results, including model
responses and other nondeterministic inputs. Pending and completed effects need
correlation identities and a restoration policy. After restoration, reconcile
outstanding operations with the host rather than blindly issuing them again. A
heap journal by itself does not establish exactly-once network behavior.

Forking should explicitly define which external authorities and resources are
shared, rebound, or unavailable. The portable object is the guest computation
and its recorded inputs; the host recreates the available operating environment.

## Implementation sequence

1. Establish the semantic reference corpus from existing Wisp programs and
   tests. Separate evaluator compatibility from tape-format compatibility.
2. Implement word packing, schemas, tables, vat, roots, and pins. Preserve tag
   and field identities and exercise growth boundaries.
3. Port Tidy and validate cycles, sharing, relocation, vector payloads, roots,
   and external-object handling.
4. Port the evaluator, macros, binding, prompts, deep handlers, and continuation
   copying. Compare observable results, conditions, and request traces with Zig
   Wisp.
5. Add image restoration and one NXT host adapter. Run a Lisp computation until
   it requests an external value; save it; restore it in a fresh machine;
   deliver the value and continue. Compare the result with an uninterrupted run.
6. Develop operation-defined restart descriptions and UI or agent policies.
   Extend the existing handler behavior deliberately rather than changing it
   incidentally during the port.
7. Add complete mutation replay and an external-effect ledger. Verify that a
   checkpoint plus journal reconstructs the chosen state, including across
   collection boundaries.

The first integrated demonstration should be small: one computation, one
suspended request, one saved image, and a resumed result. Behavioral rules and
the voice writing environment can build on that demonstrated boundary.

## First C++ heap slice

[`src/wisp/heap.hpp`](../../src/wisp/heap.hpp) now provides the storage slice,
with tests in [`test/wisp-test.cpp`](../../test/wisp-test.cpp). It has the ten table schemas,
packed words, byte and word pools, roots, pins, continuation-frame copying, and
a Tidy-style moving collector. The evaluator and source-loading slices below
build on it; tape I/O, journaling, and the NXT event-loop adapter remain
unimplemented.

The reference is `core/word.zig`, `core/heap.zig`, `core/tidy.zig`, and
`core/step.zig` in the Wisp revision linked above. The later
[current reference checkout](https://github.com/mbrock/wisp/commit/223535633179cdf2a49391820bdab16a5db5bf4e)
has the same heap layout and copying rules; its evaluator also pre-expands
lambda bodies and uses collectible effect sentinels. Those evaluator changes
must be considered when selecting the evaluator compatibility baseline.
The ported sources retain Wisp's AGPL-3.0-or-later license notice.

The first storage choices and contracts are:

- `orb` in Zig Wisp means allocator, not scheduler. The C++ vat owns one
  `nxtrt::rack<word>` per column. Capacity is raw land; append constructs rows.
  Farms and firm frame arenas still manage host slots and coroutine frames,
  not guest reachability or Lisp control state.
- Words retain the explicit five-bit tags, 26-bit row index, one-bit era, and
  signed 31-bit fixnums. Field names and column order match Zig. Schema metadata
  distinguishes traced values from raw offsets, lengths, counters, and external
  IDs; all fields are physically words, not yet strongly typed wrappers. Neither
  tuple position nor C++ enum ordinals define a future wire format.
- Allocation does not collect. `collect()` is an explicit safepoint. Row reads
  return copies; column and payload spans are read-only borrows that expire at
  growth or collection. Mutation goes through heap operations. A nonmovable
  `root` owns a host slot rewritten by collection; a pin is a stable immediate
  ID retaining its referent until explicitly freed. The heap outlives its roots.
- Ordinary pointer equality is identity within one heap and current era. The
  era bit is a debugging aid, not a durable generation number: it wraps after
  two collections. Cross-heap pointers and unrooted stale words are invalid.
  `zap` is collector-only and cannot occupy a live row's first column.
- Collection preserves cycles and shared object identity, copies each live
  word-vector payload, and retains the entire byte pool. A shallow descriptor
  copy initially aliases vector payload, but Zig Tidy copies distinct descriptor
  payloads separately during GC. This surprising behavior is retained and tested,
  not silently reinterpreted as a permanent aliasing guarantee.
- Continuation-frame copies share lexical environments but clone argument
  accumulators for function/builtin application frames. A vector in a non-call
  frame is not automatically cloned. The evaluator uses this operation for
  prompt capture, composition, and repeated invocation.
- Before forwarding, collection reserves space for all existing rows and the
  sum of descriptor payload lengths. Unlike Zig's destructive allocating path,
  allocation failure at this stage leaves old rows, roots, and pins untouched.
  This trades conservative peak memory for a simple failure boundary; it is not
  a transactional journal. Pins scan in ID order, roots newest first, and tables
  in vat order. Exact relocated indices are not a Zig tape-compatibility claim.
- An optional nonthrowing host callback releases unreachable external rows and
  the remaining rows at heap destruction. It is a host binding, not image data;
  callers must arrange one owned external reference per row and must not reenter
  the heap from the callback.

The evaluator corpus includes equivalents of the existing `core/step.zig`
examples for repeated argument continuation invocation and a deep handler
resuming requests 2 and 3 with ten times their values, yielding 50. The C++ tests
also retain a continuation only through a host pin, collect, then invoke it in a
fresh run. The Zig argument-snapshot, deep-resumption, and pinned-callback
examples pass in the current reference checkout. These distinguish shared
store, copied control state, handler reinstatement, and host retention.

Raising into a suspended handler, yielding `(caught nope)`, is an expected result
in the Zig tests, not a verified passing baseline: the [prior Wisp verification
thread](https://ampcode.com/threads/T-01a06ebb-d635-776d-a69c-95a0134cfbac)
reports that it traps even before that thread's evaluator changes. Preserve it
as an unresolved regression rather than inferring correctness from its presence.
The source-loading slice below now exercises that high-level raise protocol in
C++, including both an ERROR prompt in the captured slice and a fallback to the
caller's ERROR prompt.

## First C++ evaluation slice

[`wisp::evaluator`](../../src/wisp/eval.hpp) steps heap-resident `run` rows.
`start(expression, environment)` creates a run; `step(run)` performs one
transition; `advance(run, budget)` returns `runnable`, `done`, or `failed`.
Zero budget only polls. Exhaustion is a normal host scheduling boundary, unlike
Zig's evaluation-limit error. A transition may scan a list or allocate, so a
step budget is not a wall-clock bound. This API does not yet schedule NXT tasks.

The evaluator roots its WISP, KEYWORD, and KEY packages, which retain interned
symbols, definitions, and closures. The host interning API is exact and
case-sensitive; the reader folds ASCII names. Package inheritance is not
implemented. The caller must root
each run and any other host-held word before collection. Allocation never
collects inside a transition. Once a transition returns, its complete guest
control state is in `run`, `ktx`, and environment rows; scratch C++ registers and
argument copies can disappear. For example, without a reader:

```cpp
wisp::heap heap;
wisp::evaluator machine{heap};
auto form = heap.cons(machine.intern("+"),
    heap.cons(wisp::fixnum(19), heap.cons(wisp::fixnum(23), wisp::nil)));
wisp::root run{heap, machine.start(form)};
while (machine.advance(run.get(), 1) == wisp::evaluation::runnable)
    heap.collect(); // Optional, explicit safepoint; updates run's word.
// Inspect status and run.err; on success run.val is fixnum(42).
```

The current semantic subset is:

- Fixnums, byte/word vectors, NIL, T, self-evaluating keywords, quotation,
  lexical lookup, and global symbol values. Only NIL is false.
- Separate symbol value and function namespaces. A call form's operator must
  be a symbol; `CALL` and `APPLY` invoke computed function values. A call
  resolves its function before evaluating arguments, left to right.
- `IF`, `DO`, and parallel `LET`: initializers run left to right in the outer
  environment, and all bindings become visible together. Final body forms are
  in tail position, including singleton `DO` (which avoids Zig's extra frame).
- `%FN` closures capture shared lexical store. `%SET!` updates the nearest
  binding, or an already-bound global. Symbol value/function setters support
  global definitions and later redefinition.
- `%MACRO-FN` receives raw forms; its result is evaluated in the caller's
  environment. Required parameters, `&OPTIONAL` with NIL defaults, `&REST`,
  and `&BODY` work for closures. Lisp-defined `FN`, `DEFUN`, `DEFMACRO`, and
  lambda-body pre-expansion come from the explicitly loaded bootstrap below.
- Checked signed fixnum addition, subtraction, multiplication, comparisons,
  identity, cons/list operations, function lookup, and environment inspection.
  Unary subtraction deliberately retains Zig's identity behavior.

Language failures are heap condition vectors; builtin failures wrap their
cause. The control slice below delivers these through `ERROR` prompts, stopping
the run in `run.err` if delivery fails. Exact condition payload parity is not
claimed. Dotted or cyclic argument lists and malformed bindings fail instead of
accessing invalid native storage. Heap API contracts still apply to host-supplied
pointers, environments, and machine rows. Host allocation exceptions escape;
interrupted steps are not transactional or promised retryable. Internal
NAH/ZAP/TOP markers are not accepted as literal expressions. Builtin indices are
local to this subset, not Zig tape indices or a stable image ABI.

[`test/wisp-eval-test.cpp`](../../test/wisp-eval-test.cpp) constructs forms
directly, collects between individual steps, and checks scope restoration,
shared mutation, macro expansion scope, argument accumulation, terminal errors,
and bounded live continuation depth over 1,000 tail calls. Selected equivalent
programs were also run against the linked Zig reference with Zig 0.16.0:
argument order `(1 91 2 2)`, parallel LET `((3 40 9) 40)`, shared closures
`(17 17 999)`, redefinition `(19 7 31)`, macro expansion `11`, and optional/rest
arguments. Compare negative fixnums as words: that reference's reader treats
`-7` as a symbol and its printer renders the computed negative value unsigned.

## C++ control and builtin declarations

Builtin metadata is now derived from C++23 member-function signatures, rather
than a separate arity table and dispatch switch. `builtin::bind<&method>(name)`
generates a typed dispatch thunk at compile time: each `word` parameter consumes
one argument, and a trailing `values` span consumes the rest. For example,
`subtract(word first, values rest)` states its minimum arity directly. Unsupported
parameter types and misplaced rest spans are compile-time errors. Template
parameters also select schema fields for symbol setters and continuation
inspection. This is the portable counterpart to Zig's comptime jet declarations;
it uses neither erased function-pointer casts nor C++26 reflection.

The evaluator now also implements:

- `SET-SYMBOL-DYNAMIC!` and `CALL-WITH-BINDING`. Marked symbols search the
  continuation's nearest `BINDING` frame before lexical/global lookup or
  assignment. Unmarked symbols ignore those frames. Exiting a binding restores
  the previous scope.
- `CALL-WITH-PROMPT` and `SEND-WITH-DEFAULT!`. Tags match by identity. Sending
  captures the frames inside the nearest matching prompt, excludes the prompt
  itself, and calls its handler with the request and captured continuation in
  the outside context. No match returns the supplied default. Normal thunk
  return removes the delimiter without calling its handler.
- Multi-shot `CALL`/`APPLY` of a continuation, with exactly one value. Invocation
  copies its frames and appends the caller's context; it does not discard the
  caller. Application accumulators are independent, lexical cells remain shared,
  and dynamic binding values are copied with their frames. The captured original
  remains reusable.
- `SEND-TO-WITH-DEFAULT!`, which searches a captured continuation rather than the
  current one. It invokes the found handler with a copy of the inside frames,
  composing the captured outside frames with the current caller's context.
- `GET/CC`, `COMPOSE-CONTINUATION`, `KTX-*`, `TOP?`, and `EVAL`. As in Zig,
  `GET/CC` exposes the live context, not an immutable snapshot. Composition copies
  its argument's frames and attaches the live caller context. `EVAL` evaluates
  the supplied form in the current environment.
- Resumable language failures via the nearest `ERROR` prompt. Its handler can
  supply a replacement by calling the captured continuation. With no handler,
  `run.err` becomes `[UNHANDLED-ERROR, ERROR, condition]`. An error during a guest
  handler's subsequent evaluation searches outside that handler's removed
  delimiter. A failure while entering the handler itself is terminal, as in the
  current Zig error-delivery path.

Low-level prompts are shallow: resuming does not reinstall their handler. A
guest-defined recursive wrapper reinstalls the prompt for deep resumption,
matching `CALL-WITH-EFFECT-HANDLER` in `base.wisp`. The source-loading slice
now supplies the guest wrapper, its raise closure, and error helpers.

Two deliberate safety differences from Zig are covered by tests: prompt lookup
requires a `PROMPT` frame, rather than treating every frame whose accumulator
equals the tag as a delimiter; `KTX-POS` returns zero for an empty vector
accumulator, which is legal as a prompt tag, rather than indexing past its end.

Control tests collect between every guest step. Equivalent programs run against
the linked Zig checkout produced `((11 1) (11 2))` for shared lexical versus
copied dynamic mutation, 27 for shallow resumption, 1038 for composing both
outside contexts when sending into a continuation, and `(5 42 7)` for supplying
an unbound variable's replacement. The C++ deep-resumption test produces 50.

## C++ source loading and guest library

[`wisp::reader`](../../src/wisp/reader.hpp) owns its UTF-8 source and returns
one unrooted form at a time, with EOF distinct from NIL. Its cursor and
`read_error::offset` are byte offsets. Parsing uses explicit frames, not the
native stack; it retains no guest words between calls. It never collects, so
the caller roots each result before a safepoint. Malformed input throws a read
error rather than trapping or hanging. A final line comment may end at EOF.

Successful reference syntax is preserved, including ASCII-only case folding,
unsigned digit-prefix splitting, `-7` as a symbol, and dot-at-list-tail behavior.
Tabs and CR remain invalid between forms; symbol bars/backslashes have no escape
meaning; the reference's ordinary-symbol classification still shadows tilde
key syntax. Characters consume one Unicode scalar. String escapes are quote,
backslash, and newline only. String streams use this UTF-8 reader rather than
the reference's bytewise string-stream reader.

[`wisp::print`](../../src/wisp/printer.hpp) is a compact diagnostic printer,
not serialization. It uses an explicit work stack and active-path cycle
detection, prints repeated acyclic sharing normally, and emits `#<CYCLE>` on
recursive edges. The heap is unchanged. Signed fixnums correct the reference's
unsigned-negative printing bug; string quotes, backslashes, and newlines are
escaped. Vectors retain `#<...>` notation; characters, pins, and jets use numeric
diagnostics. Symbols use a fixed WISP context, with KEYWORD colon prefixes and
bare KEY names. Not every printed value is readable, and reading does not
promise to preserve identity or sharing.

The typed builtin registry now includes pair mutation, vector operations,
byte-string operations, floor division and positive-divisor modulo, type and
jet inspection, one-step macro expansion, closure code/name/call-count access,
package queries and exact interning, fresh keys, pin release, and run inspection.
Templated field accessors select schema tags and fields. The evaluator retains
rooted control-symbol identities so transitions do not repeatedly scan the
growing package list. Public `intern(name)` and `intern(name, package)` retain
their exact-case behavior; no package inheritance or current-package API is
implied.

Additional deliberate differences and boundaries are:

- `GENKEY!`/`FRESH-SYMBOL!` use a per-evaluator 48-bit serial in the reference's
  little-endian ZB32 spelling with a fixed epoch date. They skip preinterned
  names, produce self-evaluating KEY symbols, and require no host randomness or
  clock. They do not promise cross-machine uniqueness or reader round-trip
  identity.
- `SET-FUNCTION-NAME!` completes and returns its closure, fixing the missing
  result in Zig. Closure names must be NIL or actual symbols, including in
  `%FN`; malformed names cannot later crash diagnostic printing.
- `TYPE-OF` reports TOP as CONTINUATION and rejects other machine-only system
  markers. Empty division, invalid/reversed slices, and malformed/cyclic lists
  raise conditions. Repeated pin release is idempotent. `RUN-VAL` exposes the
  machine's raw value field; its unfinished NAH marker is not an ordinary guest
  result. Use `RUN-EXP` to inspect pending evaluation.
- `READ-FROM-STRING` reads one form and signals END-OF-FILE on empty input;
  `READ-MANY-FROM-STRING` returns a proper list. String streams are validated
  `[STRING-INPUT-STREAM, byte-offset, string]` vectors, returning `(value)` or NIL
  at EOF. Parse failures become READ-ERROR conditions and do not advance a
  stream cursor; allocation and interning are not rolled back.

[`wisp::loader`](../../src/wisp/load.hpp) reads and evaluates top-level forms
in order. Reading one form or stepping one transition consumes a unit of its
budget. Zero only polls; a read or evaluation failure stops before later forms.
The loader owns the source and roots its current run, permitting collection
between calls. Its source/cursor are host state, not portable image data.

`base_library()` exposes the embedded [`base.wisp`](../../src/wisp/base.wisp),
ported from the pinned reference. Loading is explicit, not an evaluator
constructor side effect. The library supplies definitions, quasiquotation,
condition helpers, dynamic binding macros, deep effects, and bounded eager
macro expansion. Definition/compiler diagnostic prints are omitted. Stream
effects remain guest code; unhandled process-I/O fallbacks signal
HOST-IO-UNAVAILABLE rather than acquiring implicit stdin/stdout authority.
`UNHANDLED-ERROR` re-signals at the caller, allowing a raise that misses ERROR
inside a captured slice to reach an outside handler.

The source tests boot the library and collect between loading turns. They check
optional/rest arguments, quasiquote splicing, eager standalone lambdas, walking
callback bodies, a shared branching-expansion budget, deep resumption (50),
outside and inside raises (`(CAUGHT NOPE)` and `(INSIDE REMOTE)`), a saved resume
closure across collection, UTF-8 byte cursors, and dynamically handled output.
The eager-lambda, callback-walking, branching-budget, and dynamic-output
programs also pass against the pinned Zig reference. Reader/printer tests cover
deep structures, malformed UTF-8, heap growth, cycles, and host input lifetime.
Only the three full-bootstrap integration tests opt into a ten-second debug
deadline; ordinary unit tests retain their one-second deadline.

Remaining increments include package creation/current-package/inheritance,
guest `GC` and `STEP!` safepoint policy, host debugging/tracing, tape I/O,
journaling, and the NXT event-loop adapter. Some loaded Lisp helpers refer to
these absent capabilities and cannot yet be used. No changes to NXT's host
task/deed/firm/exec semantics are part of this slice.

## Open design choices

- Independent column allocations or a packed table allocation behind the same
  schema API.
- Compatibility with current tape bytes, and the encoding and migration rules
  for later schemas.
- Retaining in-row forwarding or introducing a separate forwarding map after
  collector parity.
- Borrowed-view lifetimes, host rooting APIs, and controlled mutation access.
- The exact restart interface, decline behavior, request identities, and
  restored external capabilities.
- Whether behavioral rules eventually execute within the guest machine, in NXT,
  or at both levels with an explicit bridge.

The project direction is established: preserve Wisp's Lisp and portable control
state, use modern C++ for the implementation, and integrate through NXT's
existing host machinery. The choices above remain experiments rather than
commitments.

 ## Related RFCs

- [RFC 0000: Prolegomena to NXT System Theory](rfc-0000-prolegomena.md) supplies
  the distinction between stored state, requests, and execution.
- [RFC 0002: Firm Frame Arenas](../cur/rfc-0002-firm-frame-arenas.md) describes
  host frame territory and ownership.
- [RFC 0004: Wand Completion Routing](rfc-0004-wand-completion-routing.md) keeps
  guest identities independent of host execution-record routing.
- [RFC 0010: Firm Buffer Groups and I/O
  Land](rfc-0010-firm-buffer-groups-and-io-land.md) explores the authority to
  hold and use I/O storage.
- [RFC 0015: Async RAII Resources](rfc-0015-async-raii-resources.md) proposes
  scoped acquisition, readiness, and asynchronous teardown.
- [State of the Frontier](rfc-0017-state-of-the-frontier.md) distinguishes
  implemented NXT machinery from proposed vocabulary and mechanisms.
