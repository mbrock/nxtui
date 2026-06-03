# RFC 0003: Deck Task Registry and Task IDs {#rfc_deck_task_registry}

Status: new

## Summary

The deck should own a registry of all tasks known to it, and the hot scheduling
identity should be a compact `task_id`, not a raw coroutine handle.

The coroutine frame still belongs to firm land, as proposed in
[RFC 0002](rfc-0002-firm-frame-arenas.md). The deck owns the civic registry:
which tasks exist, which are ready, which are parked, which firm owns them, and
which continuation should be resumed when they settle.

Ready queues should contain `task_id` values. Backend completions and internal
synchronization should route to `task_id` values. Raw coroutine handles become
registry data, not public scheduling currency.

## Motivation

The current deck is intentionally small. It owns a `std::deque<ready_item>`;
each `ready_item` contains a coroutine handle plus a promise pointer. That is a
good seed runtime, and it makes the one-round pump rule very clear. See
[Runtime Overview](../../docs/rt-overview.md) and
[rt-holding / deck](../../docs/rt-holding.md).

But several upcoming changes want a durable task identity:

- direct one-shot wand completion routing;
- deck-local channel wait slots;
- better debug dumps of ready, parked, and blocked tasks;
- per-task cancellation and await slots;
- task metadata that does not live in every coroutine promise;
- generation checks when stale completions or stale handles arrive.

`task_id` already exists in [ids.hpp](../../src/nxtrt/ids.hpp), and current
promises already receive an id from a process-global `task_id_source` in
[task.hpp](../../src/nxtrt/task.hpp). This RFC makes that identity deck-owned
and operational.

## Current Shape

Current task identity is mostly observational:

- `detail::promise_base` stores `task_id id`.
- `deck::current_task_id()` reads the current promise id from the ambient
  environment.
- `deck::runtime_dump_text()` reports ready task IDs by walking queued
  promise pointers.
- `debug::park_task()` records parked wishes by task id.

Current scheduling is handle-based:

```cpp
struct ready_item {
    std::coroutine_handle<> handle;
    detail::promise_base * promise;
};
```

This is simple, but it means the deck does not have an authoritative table of
task state. A wand completion cannot simply say "task 17 is ready"; it must
hold or recover a continuation handle through an exec record.

## Proposal

Introduce a deck-owned task table over explicit borrowed storage. The task
table is runtime royalty: bounded, prominent, and declared by the caller or
root runtime instead of grown accidentally from the heap.

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

The registry row, not the promise, should eventually own hot lifecycle state:

- ready, running, parked, completed, destroyed;
- current firm;
- current completion target;
- current awaited object or await slot;
- stop requested;
- parked debug description;
- frame pointer and promise pointer.

The promise can still own type-specific result storage and compiler-required
customization points. It stops being the general task metadata record.

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

A task id should be small enough to pack comfortably into backend tickets while
still protecting against stale completions. The attractive target is a 32-bit
id:

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

The exact bit split is still not binding. The important property is that a
completion, deed, or channel waiter can name a task without owning a coroutine
handle and can be rejected if the table slot has been reused.

Forked tasks should probably not be encoded as negative IDs or special index
ranges, even though that is a tempting bit trick. A forked child is still a
task. What differs is its completion target: an awaited task resumes an
awaiting task, while a forked task publishes into its firm. That can be modeled
as a typed field:

```cpp
using completion_target = std::variant<task_id, firm_completion_port>;
```

If `completion_target` names the owning firm's completion port, the task is
background child work in that firm. The id can stay purely about table identity.

## Invariants

At most one live task occupies a given `(index, generation)` identity.

A ready queue item is valid only if its `task_id` still resolves to a live
task in a ready-compatible state.

A parked task is not also ready. This mirrors the existing runtime model
invariant in [runtime.rkt](../../nxtrt/runtime.rkt): an exec in parked state
does not have its continuation task in the deck's ready set.

The deck registry does not own coroutine frame memory. It names frames located
in firm-owned frame land. The registry storage itself is explicit deck land,
normally borrowed by the deck from its root runtime or caller.

## Migration Sketch

1. Add a deck task table over borrowed storage while still keeping
   handle-based `ready_item`.
2. Register tasks when they are started, awaited, or forked.
3. Teach debug dumps to read from the table.
4. Change the ready queue from `ready_item` to `task_id`.
5. Move completion-target and parked metadata from promises into table rows.
6. Teach wands and internal synchronization objects to enqueue `task_id`.

This can be incremental because the first table row can simply mirror the
handle and promise pointer already carried by `ready_item`.

## Relationship To Other RFCs

[RFC 0002](rfc-0002-firm-frame-arenas.md) gives the frame a firm-owned memory
home. This RFC gives the task a deck-owned civic identity.

[RFC 0004](rfc-0004-wand-completion-routing.md) depends on this registry so
one-shot CQEs can wake a task directly.

[RFC 0008](rfc-0008-pushfeed-channels-and-removing-bell-wire.md) uses task IDs
for deck-local producer and consumer wait slots.

[RFC 0013](rfc-0013-runtime-env-core-fields.md) moves `current_task_id` into
the hot runtime environment path.

## Open Questions

- What borrowed storage shape should the task table use, and how should a root
  runtime declare its capacity?
- Which fields remain in `promise_base`, and which move into the task table?
- Is `u24 index + u8 era` enough for the first generation, and how should the
  helpers hide the bit packing?
- Do root tasks and forked tasks occupy the same table, or do root tasks get a
  distinguished firm/root row?
- What small types make the registry API hard to misuse without turning it
  into ceremony?

## References

- [RFC 0002: Firm Frame Arenas](rfc-0002-firm-frame-arenas.md)
- [RFC 0004: Wand Completion Routing without exec Hub](rfc-0004-wand-completion-routing.md)
- [Runtime Overview](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [deck.hpp](../../src/nxtrt/deck.hpp)
- [task.hpp](../../src/nxtrt/task.hpp)
- [ids.hpp](../../src/nxtrt/ids.hpp)
- [debug.hpp](../../src/nxtrt/debug.hpp)
- [runtime.rkt](../../nxtrt/runtime.rkt)
