# RFC 0003: Deck Task Registry and Task IDs {#rfc_deck_task_registry}

Status: core registry implemented; broader routing/metadata changes remain
proposals

## Implemented boundary

The [deck](../../src/nxtrt/deck.hpp) has a task table with owned default backing
or explicit borrowed backing. Its ready queue is a `std::deque<task_id>`.
Registration and resolution live in [task/runtime.hpp](../../src/nxtrt/task/runtime.hpp).
IDs use a 24-bit one-based index and an 8-bit era; unregistration frees a row
and advances its era. The finite era is not protection against indefinitely
retained references across arbitrary reuse.

Registry rows contain identity, handle, promise pointer, live/vacant state and
era. They do not yet own the entire lifecycle described in the original proposal
below. Continuations, stop state, environment and completion observers still
live in promises; wands still retain backend exec records and `need` continuations.

Firms and pools use the same registry. A pool slot can own a task whose frame
comes from an ambient firm, without that task being a forked firm child. Task
identity, frame provision and structured ownership must not be conflated; see
[Recipes, pools, and structured async](../../docs/rt-concurrency-direction.md).

## Summary

The deck owns a registry of tasks scheduled through it, and the hot scheduling
identity is a compact `task_id`, not a raw coroutine handle.

The coroutine frame still occupies firm-provided land as described in
[RFC 0002](rfc-0002-firm-frame-arenas.md). The deck owns identity and ready
scheduling, not the task handle or frame memory. Recording all parked,
continuation and ownership metadata there was a broader proposal, not the
current registry's responsibility.

Ready queues contain `task_id` values. Routing backend completions and internal
synchronization directly by identity remains further work, rather than an
implemented elimination of handle/promise pairs.

## Motivation

The original deck owned a `std::deque<ready_item>`; each `ready_item` contained
a coroutine handle plus a promise pointer. That seed runtime made the
one-round pump rule clear, and motivated moving identity into a registry. See
[Runtime Overview](../../docs/rt-overview.md) and
[rt-holding / deck](../../docs/rt-holding.md).

Several uses motivated a registry identity:

- direct one-shot wand completion routing;
- deck-local channel wait slots;
- better debug dumps of ready, parked, and blocked tasks;
- per-task cancellation and await slots;
- task metadata that does not live in every coroutine promise;
- generation checks when stale completions or stale handles arrive.

Before the registry, promises received observational IDs from a process-global
source. The [current `task_id`](../../src/nxtrt/ids.hpp) is instead a deck table
identity assigned when execution is registered.

## Original starting point (historical)

Originally task identity was mostly observational:

- `detail::promise_base` stores `task_id id`.
- `deck::current_task_id()` reads the current promise id from the ambient
  environment.
- `deck::runtime_dump_text()` reports ready task IDs by walking queued
  promise pointers.
- `debug::park_task()` records parked wishes by task id.

Scheduling was handle-based:

```cpp
struct ready_item {
    std::coroutine_handle<> handle;
    detail::promise_base * promise;
};
```

That representation had no authoritative registry row. Backend completions
still hold or recover continuations through exec records today; implementing
the registry alone did not replace backend lifecycle ownership.

## Original broader proposal

The implemented task table supports explicit borrowed storage or a bounded
owned default. Its capacity is a runtime budget distinct from pool admission,
which does not count every nested await or controller coroutine.

The following layouts and completion-target types are design sketches, not
current API declarations. Further metadata migration should demonstrate a
concrete need rather than duplicate ownership or lifecycle state.

A `task_id` is an index plus a small era/generation, or another compact
representation with equivalent stale-reference protection.

The table may start as an array-of-structs, but the intended hot layout is
cache-friendly and can later become structure-of-arrays:

```text
state[]
generation[]
firm_id[]
frame_ptr[]
promise_ptr[]
continuation_id[]
await_slot[]
parked_reason[]
```

Ready queues contain `task_id`:

```cpp
ring_queue<task_id> ready;
```

The deck resumes a task by looking up its registry row, restoring that task's
runtime environment, and resuming the frame pointer recorded there.

The original proposal considered moving these fields from promises into rows:

- ready, running, parked, completed, destroyed;
- current firm;
- current completion target;
- current awaited object or await slot;
- stop requested;
- parked debug description;
- frame pointer and promise pointer.

The promise necessarily provides compiler customization points and typed
results. Whether moving more control metadata out of it simplifies the runtime
remains open; it has not been required for direct pool task ownership.

The table API should use small named types rather than raw integers wherever
the type system can carry intent:

```cpp
struct task_index { std::uint32_t value; };
struct task_era { std::uint8_t value; };
struct task_id { std::uint32_t bits; };
struct await_slot_era { std::uint32_t value; };
```

The exact names are placeholders. The point is to keep index, era, slot, firm,
and completion-target concepts from collapsing into anonymous integers.

## Task ID Shape

A task id is a compact 32-bit index/era pair. Conceptually:

```cpp
struct task_id {
    uint32_t index : 24;
    uint32_t era   : 8;
};
```

Equivalently, the stored representation can be a single `std::uint32_t` with
helpers that pack and unpack the fields. That is probably friendlier C++ than
public bitfields.

The reason to prefer 32 bits is not only compactness. It gives io_uring and
other backends a natural 64-bit ticket shape:

```text
u32 task_id
u32 slot / operation / flags
```

The implementation uses this split behind packing helpers. A stale identity
can be rejected while its era differs from the current row; an eight-bit era
eventually wraps. Durable guest identity or arbitrarily retained result handles
must not assume this is a globally unique, everlasting identifier.

Forked tasks and pool jobs are ordinary registry tasks, not special index
ranges. Their completion observers differ. A historical routing sketch was:

```cpp
using completion_target = std::variant<task_id, firm_completion_port>;
```

There is no implemented `firm_completion_port` or variant of this shape. The
current generic promise completion observer notifies either a firm child record
or a pool owner; awaiting continuations remain separately represented. The id
stays purely about table identity.

## Invariants

At most one live task occupies a given `(index, generation)` identity.

A ready queue item is actionable only if its `task_id` still resolves to a live
row with a resumable handle. The current row state is live/vacant, not a full
ready/running/parked lifecycle enum.

A parked task is not also ready. This mirrors the existing runtime model
invariant in [runtime.rkt](../../nxtrt/runtime.rkt): an exec in parked state
does not have its continuation task in the deck's ready set.

The deck registry does not own coroutine frame memory. It names frames located
in firm-owned or firm-borrowed frame land. Registry backing can itself be owned
by the deck or explicitly borrowed from its caller.

## Original migration sketch

1. Add a deck task table over borrowed storage while still keeping
   handle-based `ready_item`.
2. Register tasks when they are started, awaited, or forked.
3. Teach debug dumps to read from the table.
4. Change the ready queue from `ready_item` to `task_id`.
5. Move completion-target and parked metadata from promises into table rows.
6. Teach wands and internal synchronization objects to enqueue `task_id`.

Registration, ID-based ready queuing, and ready-ID diagnostics are implemented.
The later metadata and backend-routing stages remain proposals.

## Relationship To Other RFCs

[RFC 0002](rfc-0002-firm-frame-arenas.md) gives the frame a firm-owned memory
home. This RFC gives the task a deck-owned civic identity.

[RFC 0004](../new/rfc-0004-wand-completion-routing.md) depends on this registry so
one-shot CQEs can wake a task directly.

[RFC 0008](../new/rfc-0008-pushfeed-channels-and-removing-bell-wire.md) uses task IDs
for deck-local producer and consumer wait slots.

[RFC 0013](rfc-0013-runtime-env-core-fields.md) describes direct environment
fields. Current task identity is read through the current promise.

## Open Questions

- Is the current owned/borrowed table capacity API sufficient for composition
  controllers as well as application jobs?
- Which fields remain in `promise_base`, and which move into the task table?
- How should very long-lived references be handled beyond the current finite
  era check?
- Can completion routing be simplified without creating parallel ownership
  state for firms, pools, and backend operations?
- What small types make the registry API hard to misuse without turning it
  into ceremony?

## References

- [RFC 0002: Firm Frame Arenas](rfc-0002-firm-frame-arenas.md)
- [RFC 0004: Wand Completion Routing without exec Hub](../new/rfc-0004-wand-completion-routing.md)
- [Runtime Overview](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [deck.hpp](../../src/nxtrt/deck.hpp)
- [task.hpp](../../src/nxtrt/task.hpp)
- [ids.hpp](../../src/nxtrt/ids.hpp)
- [debug.hpp](../../src/nxtrt/debug.hpp)
- [runtime.rkt](../../nxtrt/runtime.rkt)
