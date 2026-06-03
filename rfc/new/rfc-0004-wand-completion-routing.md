# RFC 0004: Wand Completion Routing without exec Hub {#rfc_wand_completion_routing}

Status: new

## Summary

One-shot awaited wishes should route completions directly to the awaiting task,
instead of allocating a universal wand `exec` record for every operation.

The current uring and kqueue wands store each awaited wish in an address-stable
`boost::container::hub<exec>`. The kernel or platform event record points back
to that exec, and the exec contains enough state to fulfill the parked task.

This RFC keeps the wand as the owner of platform machinery, but changes the
default one-shot path:

```text
CQE/user_data -> task_id + await_slot + generation -> deck enqueue
```

Long-lived, multishot, cancellation-heavy, or stateful operations may still use
wand-owned exec records. The point is to stop making an exec the default shape
of every one-shot wish.

## Motivation

The current model is explicit and debuggable. It is also heavier than the
common case needs.

For a normal one-shot operation, a task awaits exactly one awaitee at a time.
That means the task table already has a natural place for "what I am waiting
for" and "where the result should land." Once
[RFC 0003](../cur/rfc-0003-deck-task-registry.md) gives tasks stable deck-owned
identity, the wand does not need an address-stable operation object just to
remember which coroutine to resume.

The runtime's own model already separates the concepts:

- a `wish` is the request;
- an `exec` realizes a wish;
- a task is the continuation of that exec.

See [runtime.rkt](../../nxtrt/runtime.rkt) and
[rt-occurrents](../../docs/rt-occurrents.md). This RFC narrows where an
explicit exec continuant is needed. Some wishes still need one. Many one-shot
wishes can be realized as transient backend work whose durable owner is the
awaiting task slot.

## Current Shape

In [wand.hpp](../../src/nxtrt/wand.hpp):

- `wand::prepare()` allocates a shared `urge_state<T>`.
- `prep()` creates backend state and returns a `coin_t`.
- `urge<T>::await_ready()` is always false.
- `urge<T>::await_suspend()` parks the current coroutine in the wand with a
  `need`.
- `urge_state<T>` stores the result until `await_resume()`.

In [wand/uring.hpp](../../src/nxtrt/wand/uring.hpp):

- each wish becomes a hub-stored `exec`;
- SQE/CQE `user_data` encodes an `exec *` plus CQE kind;
- `exec::state` moves through prepared, parked, submitted, settled, retired;
- cancellation has its own parked and settled phases.

That shape is still useful for complex operations. It is too much ceremony for
"read completed, wake task N."

## Proposal

Add a per-task await slot to the deck task registry:

```text
task.await_slot = {
    era,
    kind,
    completion_sink,
    cancel_phase,
    backend_flags,
}
```

An await slot is not typed result storage. It is a small routing cell saying:

```text
this task is currently parked on one backend realization;
only a completion carrying this slot era may wake it;
when the completion arrives, send it through this completion sink;
then mark the task ready.
```

The typed value still belongs in the coroutine/promise/awaiter machinery. For
example, a read awaiter may own or point at the typed place where a
`std::size_t`, exception, or borrowed buffer loan will be written. The task
table stores the validated route to that sink, not the result itself.

When a task awaits a one-shot wish, the wand realizes it into backend
submission state whose completion key contains:

```text
task_id
await_slot_generation
cqe kind
backend flags
```

For io_uring, that key is packed into SQE `user_data`. On completion, the wand:

1. decodes the key;
2. validates the task id and await-slot generation;
3. interprets the CQE result through the slot's completion sink;
4. clears or advances the slot;
5. enqueues the `task_id` on the deck.

The task resumes and reads its result from its own awaiter/promise state, not
from a shared `urge_state` owned by a wand exec and not from the task table.

## What The Wand Still Owns

The wand remains the owner of:

- the io_uring, kqueue, epoll, or other backend instance;
- SQ/CQ pumping and batching policy;
- submission staging;
- platform-specific cancellation mechanics;
- interpretation of backend completion records;
- multishot and other long-lived backend state.

This RFC removes the universal one-shot exec table. It does not move platform
machinery into the deck.

## Cancellation

Cancellation is the hardest part of this RFC.

The current uring code has explicit phases for queued, submitted,
cancel-queued, cancel-submitted, cancel-drained, and waiting-cancel-CQE. A
direct-routing design still needs equivalent state, but it can often live in
the task await slot instead of in a standalone exec.

If the cancellation protocol requires multiple backend completions after the
task has already resumed, a small wand-side record may still be required. The
rule should be:

Use task await slots for the one-shot operation state. Allocate a wand exec
only when backend state must outlive the task's parked wait or when more than
one completion must be correlated.

## Await Arity

A task should have one ordinary await slot. That keeps task await semantics
unary: a task is parked on one thing, then it resumes with one result path.

Waiting for several tasks or wishes at once is a structured-concurrency
operation. It should be expressed with a firm, child tasks, deeds, and join
policy rather than by giving one task several unrelated backend await slots.

The exception is internal bookkeeping for cancellation or final drain. If a
backend truly needs to correlate more than one platform completion after the
task has resumed, that state belongs in a small wand-owned drain record, not in
extra user-visible await slots.

## Relationship To Other RFCs

[RFC 0003](../cur/rfc-0003-deck-task-registry.md) is a prerequisite. Without a durable
task registry, user_data cannot safely name a task.

[RFC 0009](rfc-0009-wishes-urges-and-provided-buffers.md) changes what a wish
contains before realization.

[RFC 0011](rfc-0011-multishot-wishes-as-feeds.md) is the main exception: a
multishot wish is naturally a producer of feed items and probably does need
long-lived wand-side realization state.

## Open Questions

- What is the exact bit layout for packed uring `user_data`?
- What is the minimum `completion_sink` shape: function pointer, small vtable,
  or typed promise hook?
- Which existing uring cancellation phases can move into the task slot, and
  which require wand-owned drain records?
- How should kqueue and epoll map the same key shape?

## References

- [RFC 0003: Deck Task Registry and Task IDs](../cur/rfc-0003-deck-task-registry.md)
- [RFC 0009: Wishes, Urges, and Provided Buffers](rfc-0009-wishes-urges-and-provided-buffers.md)
- [Runtime Overview / Wands](../../docs/rt-overview.md)
- [Behavioral threads as occurrent structure](../../docs/rt-occurrents.md)
- [wand.hpp](../../src/nxtrt/wand.hpp)
- [wand/uring.hpp](../../src/nxtrt/wand/uring.hpp)
- [exec_lifecycle.hpp](../../src/nxtrt/exec_lifecycle.hpp)
- [raw_uring.hpp](../../src/nxtrt/raw_uring.hpp)
- [runtime.rkt](../../nxtrt/runtime.rkt)
