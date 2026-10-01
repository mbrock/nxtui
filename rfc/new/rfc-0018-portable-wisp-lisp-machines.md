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
build on it, with a cooperative NXT driver and portable tape I/O. Journaling
remains unimplemented.

The original reference is `core/word.zig`, `core/heap.zig`, `core/tidy.zig`, and
`core/step.zig` in the Wisp revision linked above. The later
[evaluator baseline](https://github.com/mbrock/wisp/commit/223535633179cdf2a49391820bdab16a5db5bf4e)
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
- Collection preserves cycles and shared object identity and copies each live
  byte- and word-vector payload. Unlike Zig Tidy, dead byte payloads are
  reclaimed, making repeated string allocation viable in a long-lived host.
  A shallow descriptor
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
step budget is not a wall-clock bound. The evaluator remains independent of
NXT; the separate driver described below supplies cooperative scheduling.

The evaluator roots its package registry and current package, initially WISP,
alongside KEYWORD and KEY. Packages retain interned symbols, definitions, and
closures. The host interning API is exact and case-sensitive; the reader folds
ASCII names. The caller must root each run and any other host-held word before
collection. Allocation never
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
accessing invalid native storage. Checks run again when guest mutation could
have changed sequence tails, LET accumulators/bindings, application cursors or
remaining arity, and environment spines/frame lengths. Environment keys need
not be symbols; non-symbol keys simply cannot match a lookup. `PACKAGE-SYMBOLS`
now returns a fresh spine, like `PACKAGES`, so pair mutation cannot damage the
private index used even when constructing conditions. Symbol identity is shared.
Heap API contracts still apply to fabricated pointers and private machine links.
Host allocation exceptions escape;
interrupted steps are not transactional or promised retryable. Internal
NAH/ZAP/TOP markers are not accepted as literal expressions. Builtin indices are
local to this subset, not Zig tape indices or a stable image ABI; C++ tapes
resolve their saved indices through a name/control-kind manifest.

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
  meta chain's nearest `BINDING` entry before lexical/global lookup or
  assignment. Unmarked symbols ignore those frames. Exiting a binding restores
  the previous scope.
- `CALL-WITH-PROMPT` and `SEND-WITH-DEFAULT!`. Tags match by identity. Sending
  captures the frames inside the nearest matching prompt, excludes the prompt
  itself, and calls its handler with the request and captured continuation in
  the outside context. No match returns the supplied default. Normal thunk
  return removes the delimiter without calling its handler.
- Multi-shot `CALL`/`APPLY` of a continuation, with exactly one value. Invocation
  shares its segments and appends the caller's context; it does not discard the
  caller. Application accumulators are independent, lexical cells remain shared,
  and dynamic binding updates path-copy their meta entries. The captured original
  remains reusable.
- `SEND-TO-WITH-DEFAULT!`, which searches a captured continuation rather than the
  current one. It invokes the found handler with a snapshot of the inside context,
  composing the captured outside context with the current caller's context.
- `GET/CC`, `COMPOSE-CONTINUATION`, `KTX-*`, `TOP?`, and `EVAL`. As in Zig,
  `GET/CC` exposes an immutable control snapshot. Composition shares its argument's
  segments and attaches the caller context. `EVAL` evaluates
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

Prompt lookup requires an actual `PROMPT` entry, now also enforced by Zig's
segmented implementation. The port still deliberately returns zero from
`KTX-POS` for an empty vector accumulator (legal as a prompt tag), rather than
indexing past its end.

The segmented control port follows upstream
[`e1c9a96`](https://github.com/mbrock/wisp/commit/e1c9a96f6c543c858aeafb0dac26bafd649f82a7):
`run.way` holds ordinary frames, while `run.meta` holds `PROMPT`, `BINDING`, and
invisible `RESUME` boundaries. A boundary suspends the outer segment and saves
its environment. Capture searches only boundaries and copies only the crossed
meta prefix; ordinary frames and partially evaluated argument vectors are shared
until a write requires a copy. A heap allocation watermark freezes frames in
constant time. Collection and tape restoration freeze all surviving frames.
Tail resumption adds no empty `RESUME` entry. `GET/CC` and `RUN-WAY` produce stable
snapshots; `KTX-*` projects the old flattened view without exposing `RESUME`.
Tests cover nested prompts, repeated LET/DO and argument resumption, dynamic
snapshots versus shared lexical store, `SEND-TO`, capture during resumption,
GC and image round-trips. Structural checks compare one- and 128-frame segments
and exercise 256 tail compositions; allocation depends on boundaries, not frame
depth. These checks are not a claim of universal wall-clock speedup.

Control tests exercise collection between every guest step as well as
uninterrupted execution. Equivalent programs run against the linked Zig
checkout produced `((11 1) (11 2))` for shared lexical versus
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
diagnostics. Symbols default to a WISP context; an optional current-package
argument selects another package by identity. Guest `PRINT-TO-STRING` supplies
the evaluator's current package. KEYWORD colon prefixes and bare KEY names
remain unchanged. Not every printed value is readable, and reading does not
promise to preserve identity or sharing.

The typed builtin registry now includes pair mutation, vector operations,
byte-string operations, floor division and positive-divisor modulo, type and
jet inspection, one-step macro expansion, closure code/name/call-count access,
package queries and exact interning, fresh keys, pin release, and run inspection.
Templated field accessors select schema tags and fields. The evaluator retains
rooted control-symbol identities so transitions do not repeatedly scan the
growing package list. Public `intern(name)` and `intern(name, package)` retain
their exact-case behavior. The former always uses WISP; the latter also
searches direct used packages, as described below.

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
Only the five full-bootstrap integration tests opt into a ten-second debug
deadline; ordinary unit tests retain their one-second deadline.

## Packages and cooperative host turns

Package creation and selection now include `%DEFPACKAGE`, `DEFPACKAGE`,
`PACKAGE-SET-USES!`, and `IN-PACKAGE`. The base library replaces the primitive
`DEFPACKAGE` control form with its own macro. The reader selects the current
package for each unqualified symbol, so a loader's `IN-PACKAGE` affects later
forms, not symbols already read in the same form. Host `intern(name)` remains
independent of this state; `current_package()` exposes it explicitly.

Interning searches the package's own symbols first, then each directly used
package in list order, then creates a local symbol. Inheritance is deliberately
not transitive, matching Zig. WISP's NIL/T immediate special case is not
inherited. Uses lists retain reference aliasing, but setters reject improper,
cyclic, or non-package lists before mutation; lookup rejects malformed portions
it encounters after later alias mutation. Duplicate package names raise
`PACKAGE-EXISTS` rather than relying on Zig's no-clobber assertion. `PACKAGES`
returns a fresh list spine so guest mutation cannot damage the rooted registry.

Guest `GC` sets `collection_requested()` and returns NIL. `advance` stops early
while a request is pending; `step` can still perform one transition. Neither
collects inside an evaluator transition. The host services the request with
`evaluator::collect()`, which clears it after successful collection. The loader
does this automatically between committed transitions, so all other live host
words must be rooted across loading calls, not just between them.

`STEP!` advances another heap-resident run once and returns NIL; failure remains
in the target's error field, and terminal targets are unchanged. Nested steps
commit each run before dispatching the next, using an iterative loop rather
than native recursion. Stepping an active ancestor signals `ACTIVE-EVALUATOR`.
Collection waits until the whole chain returns. A budget counts outer evaluator
calls, not every nested step, and still makes no wall-clock guarantee.

[`wisp::drive`](../../src/wisp/nxt.hpp) is a small, optional NXT coroutine
adapter. It advances a rooted run with a positive quantum, services pending
collection, and yields between unfinished turns. Taking a root by reference
also permits collection before the task's first resumption. Cancellation is
checked before each turn and leaves the guest run resumable; it does not
translate host cancellation into guest ERROR or install implicit I/O handlers.
The evaluator, heap, and root must outlive the task, and all other host-held
words must remain rooted across awaits.

[`test/wisp-runtime-test.cpp`](../../test/wisp-runtime-test.cpp) exercises
package order, shadowing, direct-only inheritance, malformed uses, a full
bootstrap with current-package changes, GC pauses, STEP! isolation and cycles,
and a 2,000-run nested stepping chain. NXT tests check sibling progress,
collection before first resumption and between turns, cancellation/resumption,
and a timer completion returning to a captured guest continuation with result
42. No changes to NXT's task/deed/firm/exec semantics are part of this adapter.

## Portable C++ tapes

[`wisp::tape`](../../src/wisp/tape.hpp) provides `encode`/`decode` for byte
buffers and blocking `write`/`read` for host binary streams. `decode` returns a
separate, address-stable `image` owning `storage`, `machine`, and the rooted
`entry`. It never replaces or mutates a running machine. Its restore-only
evaluator constructor does not install primitives over saved definitions.

The tape preserves complete column tables and byte/word pools, evaluator roots
and caches, current package, pin map/next ID, fresh-key sequence, GC-request
flag, and one caller-selected entry value. A vector or list entry can hold
several logical application roots. Sharing, cycles, closure environments, and
captured control state remain guest data. Native root registrations, loader
source/cursors, NXT tasks, and host resource ownership are not serialized.
Other host roots must be explicitly reachable from the entry, pins, or
evaluator roots to survive collection in the restored image.

Save only between evaluator calls, as for collection. Encoding does not collect
or mutate the source. Whole pools include unreachable data unless collected
first. Tidy reclaims both byte and word payloads, but a tape is still not a
sanitized data export.
Any external row causes rejection, including an unreachable row until the host
explicitly collects it. Rebinding external capabilities is deliberately absent
in this version; saving a numeric host handle would not save its resource.

### Tape version 2 has an explicit byte encoding

This is **not** the Zig `wisp tape v0.9.0` format. Version 2 adds `run.meta` and
segmented contexts; version 1 tapes are explicitly rejected, without migration.
All integers below are unsigned
32-bit little-endian unless specified otherwise. Text is a byte length followed
by exactly that many bytes, without a terminator or alignment padding.

| Order | Encoding |
| --- | --- |
| Header | Eight bytes `NXWISP\r\n`, version `2`, era `0/1`, next pin ID, 64-bit little-endian fresh-key serial, GC-request `0/1`, entry word |
| Evaluator roots | Count, then `(text name, word value)` for each saved root |
| Builtins | Count, then `(text name, control-kind 0/1)`; ordinal is the saved jet payload |
| Byte pool | Byte count, then raw bytes |
| Word pool | Word count, then little-endian words |
| Tables | Table count, then each table as described below |
| Pins | Count, then `(ID, value)` pairs; released handles may remain in guest data without map entries |
| Integrity | 32 raw SHA-256 digest bytes over every preceding byte |

Each table contains its explicit numeric tag, row count, column count, all
`(text field-name, text field-kind)` descriptions, then each complete column's
little-endian words in that description order. Field kinds are `value`,
`offset`, `length`, `count`, and `external`. Tags and schema names identify the
storage; C++ tuple order, member layout, and enum ordinals do not. Readers accept
reordered table/column/root directories but require exactly the supported
schema and root set. Writer traversal derives from `schema<T>::columns`.

Root names are WISP, KEYWORD, KEY, packages, current, NIL-name, T-name, DO, IF,
EVAL, LET, PROMPT, BINDING, CONTINUATION, RESUME, &OPTIONAL, &REST, and &BODY.
Builtin names are unique at compile time. Restore resolves names and control
kinds against the current registry, then remaps jet words in every value
field, word-pool element, root,
pin value, and entry. Raw offsets, lengths, and call counts are never remapped.
Names do not promise compatibility after a builtin's semantics change: an
incompatible language or schema change needs a version bump or explicit
migration, not silent acceptance.

Decode checks the checksum before constructing a machine, bounds counts against
remaining input before allocating, rejects trailing bytes, and validates tags,
eras, indices, pool slices, pin sequences, canonical roots, private package
indexes, and acyclic segmented control links, including wrapper registers and
boundary payloads. Mutable uses lists, environments,
syntax, and continuation payloads are checkpointable even when malformed:
the evaluator must preserve their meaning, including a condition on later use.
For example, a cyclic package uses list restores and still raises
`INVALID-PACKAGE-USES`; an application with a damaged cursor restores and
raises `INVALID-CONTINUATION`. List/continuation validation walks are
iterative and memoize shared suffixes. Default input limit is 64 MiB and can be
overridden; it is not a resident-memory or execution-time quota. Allocation and
stream exceptions propagate with RAII cleanup. SHA-256 is corruption detection,
not authentication. Load trusted checkpoints only: validation is not a proof of
Lisp program semantics or a sandbox against malicious mutable machine state.

The stream layer does not flush, close, fsync, or atomically replace files, and
it adds no guest filesystem authority. The host must choose paths, check final
close/durability errors, and perform atomic replacement when needed. A guest
checkpoint effect can later hand this work to the host at a safepoint.

[`test/wisp-tape-test.cpp`](../../test/wisp-tape-test.cpp) boots the guest
library, suspends an effect with its request and resumption closure, writes an
image, and destroys the original machine. A fresh decoded image collects and
resumes on NXT with result 42, matching uninterrupted execution; another
resumption with a different value returns 43. Other tests cover both eras,
cycles/sharing, aliased vector payloads, pins, saved definitions/current package,
fresh keys, pending GC, reordered builtin/table/column identities, malformed
images with recomputed checksums, stream failures, and deep continuations.

### The executable host owns bounded concurrent jobs

[`wisp`](../../src/wisp/main.cpp) now supplies `run`, a line-oriented REPL,
`inspect`, and `restore`. Its optional [`host.wisp`](../../src/wisp/host.wisp)
library routes console output, line/byte input, `sleep-ms`, `spawn`/`join`, and
HTTP through `send!` effects. It adds no jets or authority to the portable base.
NXT drives bounded evaluator turns, file-descriptor I/O and timer waits;
source-file loading and checkpoint replacement are explicitly blocking host
operations. Evaluator quanta bound transitions, not wall-clock time within a
primitive, so this is cooperative concurrency, not CPU/memory isolation.

The v2 executable-host schema is separate from the portable tape format:

```
[:NXT-WISP-2 source byte-offset run pending last-result jobs request-serial]
child = [:NXT-JOB NIL slot run pending result status]
pending = NIL | [id [operation arguments] deadline resume raise]
```

`jobs` is a 64-element vector; slot zero is reserved for the source job and
contains NIL in this vector. Child status is 0 running, 1 successful, 2 failed
and unobserved, or 3 failed and observed/cancelled. Treat job handles and host
records as opaque, not mutable application vectors. Successful jobs are unlinked
from the slot vector; a retained handle keeps its result. Unobserved failures
remain rooted until joined, and are reported when the other jobs finish. They
do not race with the parent's opportunity to install an error handler. Main-job
failure stops the whole session. Normal main completion waits for children.

`spawn` admits a zero-argument closure or raises `:CAPACITY`; it never waits for
a free slot. `join` may be repeated, returns the result, raises `:JOB-FAILED`,
or rejects a cycle with `:JOIN-CYCLE`. These are session-owned jobs, not lexical
structured-concurrency scopes exposed to Lisp. They share globals, packages,
and current-package state, and start with fresh dynamic contexts. Console
operations serialize per stream, including cancellation-safe ownership cleanup.

Each slot has a long-lived native worker, a rooted job, and a single-waiter
bell. Each current job has its own short-lived NXT firm and forked execution
task. This lets cancellation stop a request without destroying its reusable
worker, and avoids accumulating one deed per job in an everlasting firm.
Ordinary NXT wishes park these tasks; the platform wand owns backend execution
and cancellation/drain. Guest job/request identities are not native exec IDs.
Restore creates fresh native execs; no deck, wish, wand registration, descriptor,
or coroutine frame is serialized. No NXT lifecycle rule changes are required.

The deep guest handler writes resume/raise closures into the pending record
before the host starts the operation. The host assigns a decimal string ID from
a saved 64-bit sequence. Unlike interning a GENKEY per request, this does not
permanently retain consumed request identities in the KEY package. Language
GENKEY semantics are unchanged. All live guest words, including results returned
from native helper coroutines, are rooted across suspension and collection.
Host-triggered collection follows allocation growth at committed boundaries;
explicit guest GC requests still collect through `drive` at safe transitions.

Completion installs a callback run before removing the pending record. CLI
`restore --effects --cancel` delivers `[HOST-ERROR operation :CANCELLED message]`
through each saved pending request's raise closure. Other machine-readable codes
include `:INVALID-ARGUMENT`, `:UNSUPPORTED-OPERATION`, and `:IO`; delivery covers
both standard and cpptrace runtime exceptions. Ctrl-C still terminates the
process; it is not an automatic checkpoint or guest raise. V1 host images are
not migrated: use the previous executable for `NXT-WISP-1` images. The portable
tape format itself is unchanged.

`run SOURCE --checkpoint TAPE` freezes timers at the next newly armed timer,
then saves only when every worker is idle, parked on an armed timer, or joining
another job. Merely having a pending record is not sufficient: a native effect
may still be in flight. Existing console I/O must finish and commit before
saving. Failure to quiesce within five seconds aborts the session without writing
a tape; network listeners are rejected with `:NOT-REPLAYABLE` in checkpoint mode.
The save is synchronous, then the host stops and joins all native workers before
returning. This prevents a saved timer from also firing in the original process.
`restore --effects --checkpoint OUT` does not request a save merely because a
timer was restored; only a newly armed timer requests the next checkpoint.
Other jobs' older timers can still be pending in that next image. The source
and byte offset are in the entry, not a native reader, so a fresh process resumes
inside the current form and then reads later forms in the saved current package.
The deadline is an absolute Unix-millisecond decimal string (epoch time exceeds
a fixnum). Timer waits recheck wall time in at most one-second monotonic waits;
wall-clock adjustments affect them, and already elapsed timers fire immediately.
Files use mode 0600, same-directory temporary creation, checked writes/close,
file fsync, atomic rename, and directory fsync. A directory fsync failure after
rename reports uncertain durability; it cannot roll the rename back. No timer
means no checkpoint, reported as a CLI error. Saving does not retain an OS task.

Restore requires `--effects` before dispatching any operation. `inspect` prints
all jobs' pending requests, IDs, deadlines, and the source offset without running the guest.
Neither command implicitly updates the input tape. Enabling effects in multiple
forks can duplicate output: there is no exactly-once promise or result ledger,
and request sequence IDs are not globally unique across forks.

[`test/wisp-host-test.py`](../../test/wisp-host-test.py) exercises actual process
exit/restart, deletion of the original source, future and expired deadlines,
effect gating, cancellation/error handlers, binary input, REPL recovery, console
serialization, join cycles, 4,100 job admissions, multi-job restore, and refusing
checkpoints with in-flight stdin I/O. There is no Lisp-form stdin reader,
file capability, debugger, external-resource rebinding, or guest checkpoint
effect. Network effects are intentionally not checkpointable; add reconciliation
before enabling network-resource restore. Journaling remains separate work.
`drive` itself still supplies only scheduling, not this host's effect policy.

### C++ HTTP owns protocol policy; Wisp owns request handlers

[`nxtrt::http::serve`](../../src/nxtrt/http-server.hpp) borrows a listener and
accepts a `task<response>(request)` handler. Beast parses and serializes bytes;
NXT alone schedules I/O, timers, and cancellation. There is no Asio event loop.
One dispatcher accepts connections into fixed workers, with one waiter per bell
to avoid overlapping same-fd readiness registrations on kqueue. Per-operation
timeout scopes retire on completion rather than accumulating deeds in the
server's lifetime scope. Accepted sockets close after their operations drain;
the caller retains ownership of the listener through cancellation and join.

Integration also fixes `with_timeout`: a scope cancelled before its first turn
does not fork into a stopped firm, and an ordinary body exception is not replaced
by the timer's cancellation. The optional cpptrace exception ABI is now shared
by the core library, executables, and installed pkg-config consumers.

The policy is HTTP/1.1 origin-form (plus OPTIONS *), sequential requests per
connection, and concurrent connections. It rejects ambiguous framing, folded
headers, CONNECT, upgrades, and all Expect requests; trailer fields are discarded.
The server owns response framing, including HEAD and 204/205/304. Defaults allow
64 connections, 1,000 requests per connection, 16 KiB headers, 1 MiB request
payloads, 2 MiB body wire data, and 8 MiB responses. Whole-phase timeouts are 10s
for headers and 30s for bodies, handlers, and writes. Limits bound server
buffers, not arbitrary allocations inside a handler. Handlers must cooperate
with cancellation. TLS and public exposure belong to a reverse proxy; forwarded
headers have no trusted meaning here.

Wisp's `serve-http` returns a listener job. Each request gets an independent
guest job and dynamic `*request*`/`*response*` bindings, preserving the old
zero-argument web-handler convention. Request records contain method, raw path,
raw query string, header pairs, and binary body. `request-header` compares names
case-insensitively and returns the first matching value. Responses are
`[status headers body]`, with headers a list of `[name value]` vectors. Handler
return values are ignored; mutate the response or use `send! :respond` to exit
early. Guest failures become a generic 500, and exhausted guest slots a 503.
HTTP handler timeout cancels the corresponding guest job's firm, including
timers or console I/O; it does not abandon a still-running guest task.

[`demo/wisp-http.wisp`](../../demo/wisp-http.wisp) serves loopback port 8080.
[`test/http-server-test.cpp`](../../test/http-server-test.cpp) tests real sockets
on epoll/io_uring (and compiles a kqueue variant on those platforms), including
phase limits/timeouts, injection, pipelining, binary bodies, cancellation, fd
cleanup, and more than 4,096 requests/connections. The Linux orb does not execute
native kqueue tests. [`test/wisp-http-test.py`](../../test/wisp-http-test.py)
tests overlapping guest handlers, GC while requests wait, dynamic isolation,
binary/chunked requests, response policy, timeout cleanup and checkpoint refusal.
Streaming bodies, WebSockets, filesystem-backed sites, and the old JS/CGI bridge
are not part of this interface.

## Open design choices

- Independent column allocations or a packed table allocation behind the same
  schema API.
- An importer for legacy Zig tapes, and migration rules for later schemas.
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
