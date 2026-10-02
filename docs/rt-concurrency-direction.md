# Recipes, pools, and structured async {#rt_concurrency_direction}

This is a design synthesis, not a second runtime specification. It records the
direction established while simplifying firms and introducing bounded idea
pools, and the questions to resolve with real migrations.

The implemented baseline is the pool introduced in commit `8946acc`.
The Wisp async/HTTP simplification now uses that pool for native connections
and an explicit operation-awaiting bridge, without permanent guest workers.
See [the Wisp guide](../README.md#wisp--portable-lisp-machines-and-http-on-nxt)
and [RFC 0018](../rfc/new/rfc-0018-portable-wisp-lisp-machines.md) for its
implemented contract; guest structured concurrency remains a design question.

## What exists, and what is a direction

| Piece | Status |
| --- | --- |
| `idea<Fn>`, `idea_result_t<Fn>` | Implemented concepts/traits for concrete task- or hope-producing callables |
| `pool<Idea>` | Implemented: borrowed slots, direct task ownership, completion-order result feed |
| Firm bookkeeping | Growable nursery records; bounded ledgers and exact-N tuple admission were removed |
| Tuple concurrency helpers | Implemented over ordinary nurseries, not static teams |
| Frame provision | Still supplied by the ambient firm, including for pool jobs |
| Idea-level `cope`, generic feed mapping, lifetime-aware terminal consumers | Design direction; no APIs are specified here as already available |
| Static heterogeneous teams | Design direction, not a revived bounded-firm implementation |
| Wisp without permanent evaluation workers | Implemented explicit-await bridge and pool-based HTTP; guest structured-concurrency semantics remain open |

See @ref rt_pool "the pool guide" for the current API and executable example.

## Separate the relationships, not merely the names

Several relationships previously hid behind “this task belongs to a firm”:

1. **Recipe:** what work could be performed, with which input?
2. **Execution state:** what has started or acquired a coroutine frame?
3. **Frame provision:** whose memory holds that frame?
4. **Work ownership:** who must stop and drain it before leaving?
5. **Observation:** who may consume its eventual result or failure?
6. **Scheduling:** what makes a runnable continuation run again?

These often have the same surrounding scope, but they are not the same
relationship. A pool now owns pending task handles without registering them
as firm children; their allocations still come from ambient firm frame land.
The deck identifies and schedules those tasks without owning their frames.

The goal is shared lifetime rules, not one universal container or a configurable
holder with a policy parameter for every difference. The useful questions from
[Buffer Royalty](../rfc/new/rfc-0000-prolegomena.md) remain:
what is held, who has a claim, and what causes release?

## Ideas are recipes; tasks are already-born execution

The central distinction from @ref rfc_idea_algebra "RFC 0014" is:

```txt
wish       analyzable description of outside work
idea       concrete callable recipe, opaque until invoked
hope<T>    immediate T or pending task<T>
task<T>    an already-born coroutine frame and its owning handle
```

An idea is a concept, not inherently a `std::function` or heap-allocated erased
wrapper. A homogeneous stream can carry one concrete recipe type with different
inputs. Different recipe types producing the same result do not automatically
become the same feed element type.

This makes a feed of ideas a better admission boundary than a feed of tasks:
upstream can describe work without allocating every coroutine frame in advance.
The pool reserves capacity, takes a recipe, moves it into stable storage, and
only then invokes it. A ready hope can produce an output without a coroutine or
deck round-trip; pending work uses the normal task machinery.

The recipe's lifetime is part of the contract. A coroutine member or capturing
coroutine callable may borrow its callable object. Moving that recipe after
invocation, or destroying a temporary closure while its task runs, is unsafe.
The pool retains recipes through execution and output consumption. Recipes must
also own their input or borrow from a sufficiently long-lived owner, not from
an upstream view invalidated by taking the recipe.

The resemblance to senders is the separation of description from operation
state, not a commitment to implement the C++ sender/receiver protocol. Wishes
remain inspectable operation data; arbitrary ideas do not become inspectable
just because both can eventually produce a result.

## A pool circulates capacity

Dynamic discovery of work does not require dynamically growing concurrency.
A crawler can discover arbitrarily many URLs over time while allowing only
eight admitted operations at once.

The pool's invariant is:

```txt
free + reserved for input + running + completed/unconsumed = capacity
```

Here “running” includes admitted pending work that is prepared but not yet
scheduled, as well as tasks suspended on an await; it does not mean CPU-active.

The cycle is:

```txt
free slot → admission → execution → completed outcome → consumption → free slot
```

Slots provide stable residence for independently completing work. A completion
sequence provides temporal order, which need not match slot or input order.
The farm supplies free slot identities; the pool adds waiting, execution
ownership, and publication. `farm::alloc()` and `farm::try_alloc()` themselves
report exhaustion; they do not wait for a consumer to return capacity.

Completion is not consumption. A later pool pump may evacuate a completed
task's result into the output ring and destroy its frame, while its admission
slot remains occupied. Consumption returns that credit. If the consumer stops
reading, already admitted jobs can finish, but further admission stops.
Closing explicitly discards outcomes and returns credits after the required
drain.

This is why the removed exact-N firm bookkeeping was the wrong expression of
the bound. It counted total admissions into a nursery and eventually threw.
The pool counts outstanding obligations and reuses their residence.

### Bounds are local and deliberate

An N-slot pool is not a proof that the entire computation uses bounded memory:

- recipes and output values may own allocations;
- response bodies and upstream queues need their own budgets;
- nested awaits consume additional frame memory;
- work explicitly forked into an ambient firm is outside the pool's slot bound;
- a crawler's discovered-URL frontier and visited set are separate resources.

The appropriate principle is visible ownership and explicit capacity where
work is held, not a claim that one concurrency number bounds every resource.

Feedback also needs a progress rule. If every occupied crawler slot waits to
admit more links before completing, all slots can wait for capacity that only
their own completion/consumption could release. Separate discovery, admission,
and consumption so that releasing capacity does not require acquiring more.
Detecting that no queued or active work can discover more URLs is a property of
that feedback composition, not ordinary temporary emptiness of a feed.

## Teams and pools describe different shapes

| | Team direction | Implemented pool |
| --- | --- | --- |
| Membership | Fixed heterogeneous positions | Changing occupants of bounded slots |
| Result shape | Typed product of named/positional results | One homogeneous output type |
| Storage geometry | Tuple/product | Slots plus completion order and output ring |
| Typical use | Capture plus monitor; fixed sample fields | Requests, tool calls, connection attempts |
| Observation | Selected positional results or aggregate | Consuming a result stream |

Both can have known non-frame storage and downward borrowing. This does not
make compiler-generated coroutine frame sizes known, nor promise allocation-free
execution. The existing frame allocator uses nonmoving chunks or explicit
borrowed land; a ring with prefix retirement was rejected because long-lived
frames pin the prefix.

The current tuple helpers are not an implementation of static teams: they
permit further ambient forks. A future team must express an actual fixed work
shape and sound lifetimes, not just allocate N bookkeeping slots and call the
result structured.

## Failure is exceptional unless explicitly coped with

The desired default is simple: **task failure means pool failure**. If an
individual failure is an acceptable outcome, the recipe should explicitly cope
with it and produce a value such as `expected<T, error>`.

Today deeds have `cope()` and a catching result handle. A corresponding
idea-level adaptor is a next step, not an implemented API. Coping must happen
around individual work before an exception becomes terminal pool failure;
catching the failed output stream afterward cannot recover discarded sibling
outcomes. Cancellation and recoverable application errors must remain
distinguishable: handling an error must not bypass stop/drain obligations.
Deadline composition must also preserve ordinary failure versus deadline
expiry versus external cancellation, rather than treating all three as
interchangeable unsuccessful attempts.

Current pool error handling is consumer-driven. A final-suspend observer
records readiness, not failure policy. Reading encounters the error, stops
admission, cancels/drains the pending upstream read and admitted jobs, discards
outstanding outcomes, and rethrows. This does not generically close a borrowed
upstream producer; independently owned upstream work needs its own teardown
boundary. The pool does not collect every failure during close, and successful
unconsumed outputs from the same pump may be discarded.

By contrast, firm policies such as `stop_on_failure` react at final suspension
even when nobody is currently reading results. Do not describe those as
identical semantics. A future policy requiring prompt completion-time action
must account for this distinction rather than hiding a monitor behind a feed.

## Selection can be a feed operation; lifetime must accompany it

The intended expression of “first successful connection” is conceptually:

```txt
addresses → connection ideas → explicit coping → pool → first success
```

That is a design sketch, not current callable syntax. Filtering and selection
need not be special pool policies. The terminal consumer can select the useful
outcome and finish the computation's extent, which stops and drains losers
before their borrowed storage is released.

If every attempt fails, the terminal operation must decide how to report no
success or aggregate the failure values. Filtering out failures alone does not
preserve the existing `wait_any` helpers' all-failed diagnostics.

However, raw `take()` on a borrowed feed must remain a read: the caller may want
another value later. It cannot silently close the source. We need an explicit
ownership or close-capability boundary for terminal operations such as
first-success, collect, and drain. Not every feed owns cancellable computation,
and a composite pipeline must specify which upstream owners its teardown closes.
The current concrete spelling is `finally(consume(pool), cleanup)` with cleanup
returning `pool.close()`, and the pool/land outside the consumer's frame.

This is the ergonomic target: applications describe useful work and observation,
not repeat “enter scope, fork, retain deeds, join, unwrap, cancel losers.”
The low-level machinery still exists; it belongs under reusable operations.

Batching should survive this layering. Admit up to available capacity, publish
several ready outcomes, and use existing feed/sink transfers. Avoid converting
every element into a new allocated message or an unavoidable suspension.
Returning source credit is not the same as eventual delivery through every
downstream buffer.

## Wisp: operation awaiting is not worker admission

Two different capabilities must not be conflated:

- **Evaluator concurrency/fairness:** advance several guest computations in
  bounded evaluation quanta.
- **Async operation awaiting:** preserve a guest continuation while external
  work runs, then resume it with a value or error.

Ordinary I/O does not inherently require guest `spawn`/`join`, a registry of
evaluation jobs, or a fixed number of permanent native evaluation workers.
Wisp effects can capture unfinished guest computation as heap data. A host
bridge can await a native task, which may compose many low-level wishes, then
arrange resumption of that computation. HTTP is such a composite operation,
not necessarily one wand wish.

The executable Wisp host now uses that single-threaded async bridge, removing
the permanent worker/job layer rather than preserving it as an implicit
language contract. Single-threaded does not
mean one outstanding HTTP request: several computations can be suspended while
native operations progress. A bounded HTTP connection pool can own native
admission without becoming an evaluator-worker limit.

The current bridge awaits descriptor-selected native tasks and keeps callback
activations rooted until their native awaits drain. Guest structured-concurrency
syntax remains open. The broader design still needs to respect these boundaries:

- heap continuations are data, not another scheduler;
- native frames, roots, buffers, and backend operations still need owners;
- cancellation must drain native obligations before releasing their guest roots;
- checkpointing a continuation does not serialize a live native operation;
- replaying a multi-shot continuation must not accidentally duplicate ownership
  of one native operation;
- wish/idea construction and starting execution are distinct decisions.

Keep guest control state in the heap. Evaluation step budgets can remain a
fairness mechanism without being the admission mechanism for async I/O.
The JavaScript comparison is useful for separating continuation callbacks from
eventual-result cells; it is not a reason to import Promise eagerness or its
absence of structured cancellation.

## Migrate recognizable uses, not every class at once

The usage inventory suggests the following path. HTTP serving and the Wisp
host have now been converted as described above; the other rows remain
migration targets, not reports of completed work:

| Use | Why it fits / what must be preserved |
| --- | --- |
| [AI tool batches][tool-batches] | Homogeneous jobs; remove fork/join/deed-vector shell. Preserve input-order results and collect-before-rethrow behavior unless deliberately changed. |
| [Directory metadata][directory-metadata] | Bounded stat ideas; results are sorted afterward, so completion-order production is natural. |
| [Connection racing][connection-racing] | Coped attempts and first-success consumption. Existing range selection chooses an input-order success after drain; distinguish that from first published success. |
| [HTTP serving][http-serving] | Migrated to one accept feed and a bounded connection pool, preserving connection-local error containment. |
| [Process capture][process-capture] and [shell supervision][shell-supervision] | Heterogeneous resource lifetimes; express a team or primary activity with companions, not an artificial uniform job stream. |
| [Wisp host][wisp-host] | Migrated to explicit native-task awaiting; old guest-job identities are no longer a prerequisite for I/O. |

These source links pin the inventory to the implemented baseline, so the
comparison remains intelligible after those applications change.

[tool-batches]: https://github.com/mbrock/nxtui/blob/8946acc004295d4699a69fb3c6f79f5f15d5c69b/src/nxtai/tool_batch.hpp#L362-L391
[directory-metadata]: https://github.com/mbrock/nxtui/blob/8946acc004295d4699a69fb3c6f79f5f15d5c69b/src/nxtrt/fs.hpp#L151-L172
[connection-racing]: https://github.com/mbrock/nxtui/blob/8946acc004295d4699a69fb3c6f79f5f15d5c69b/src/nxtrt/net_dns.hpp#L43-L54
[http-serving]: https://github.com/mbrock/nxtui/blob/8946acc004295d4699a69fb3c6f79f5f15d5c69b/src/nxtrt/http-server.cpp#L348-L454
[process-capture]: https://github.com/mbrock/nxtui/blob/8946acc004295d4699a69fb3c6f79f5f15d5c69b/src/nxtai/tool_process.hpp#L128-L159
[shell-supervision]: https://github.com/mbrock/nxtui/blob/8946acc004295d4699a69fb3c6f79f5f15d5c69b/demo/shell_scope_demo.cpp#L344-L380
[wisp-host]: https://github.com/mbrock/nxtui/blob/8946acc004295d4699a69fb3c6f79f5f15d5c69b/src/wisp/main.cpp#L1029-L1128

Some existing firms are only frame/resource scopes, with no explicit child
team. Root task construction also requires an ambient firm. Replacing those
with empty pools would obscure rather than simplify the system. Separate frame
provision from child ownership before trying to delete firm wholesale.

## Next decisions and verification

Use actual pipelines to settle:

1. Concrete idea-level coping and typed outcome composition.
2. Lifetime-aware terminal consumption without surprising borrowed-feed reads.
3. Generic mapping and bounded in-memory admission/feedback, without a second
   scheduler or a mandatory channel around every source.
4. Static team representation and companion-resource lifetimes.
5. Frame provision independent of the nursery API.
6. The native task/guest continuation cancellation and root-release boundary.

Preserve evidence alongside the design: many more jobs than slots; a paused
consumer; pending input while a job completes; partial lookahead and EOF;
throwing recipe/result moves; stop during user callbacks; early consumer exit;
and cancellation that really drains backend operations. Model checks cover
bounded slot safety; they do not establish scheduler fairness or eventual
cancellation progress.

Related material: [RFC 0000](../rfc/new/rfc-0000-prolegomena.md),
@ref rfc_firm_frame_arenas "frame arenas",
@ref rfc_join_completion_feed "completion feeds",
@ref rfc_idea_algebra "idea algebra",
@ref rfc_async_raii_resources "async resources", and the earlier
[land/slot/flow discussion](../rfc/new/rfc-0017-state-of-the-frontier.md).
