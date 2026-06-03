# RFC 0005: Firm Bookkeeping without Heap Vectors {#rfc_firm_bookkeeping}

Status: current

## Summary

After firms own frame land and decks own task identity, firm bookkeeping should
move out of heap vectors and shared pointers.

A firm should borrow explicit storage for child records, deed records, join
state, cancellation state, and settlement queues. Forked children should be
known by `task_id`. Deeds should be lightweight handles to selected child
results, not the primary ownership mechanism for child work.

## Motivation

The current `firm` implementation in [task.hpp](../../src/nxtrt/task.hpp) is a
good semantic seed:

- `firm::fork()` starts child work without awaiting it immediately.
- `firm::join()` gives the scope a structured settlement point.
- `deed<T>` lets callers observe selected child results.
- `firm::stop()` requests cancellation for all children.

The storage shape is being moved in this direction incrementally. The first
implementation replaced the heap-growing child vector with bounded child slots:
`firm_child_storage_ref` and `static_firm_child_storage<N>`. Each slot contains
inline storage for one child record, and a deed carries separate result state
rather than sharing ownership of the child record itself.

Join failure collection now has the same bounded-storage shape:
`firm_join_storage_ref` and `static_firm_join_storage<N>` provide explicit
slots for the exception pointers observed while `join()` drains children. The
runtime still materializes an `exception_group` when throwing multiple failures,
but the live join bookkeeping is no longer a growable vector.

Child completion reporting also has explicit bounded storage now:
`firm_completion_storage_ref` and `static_firm_completion_storage<N>` provide
one small `child_completion` record per reported child. Final suspend records
completion without throwing; if the borrowed completion storage is too small,
`join()` reports that as a firm storage error after it has finished draining the
children it can observe.

Callers that want to grant all of a firm's local land together can use
`firm_storage_ref` or
`static_firm_storage<FrameBytes, Children, JoinFailures, Completions, Deeds>`.
That aggregate does not introduce a new owner; it is just a named bundle of the
borrowed frame, child-record, deed-record, child-completion, and join-failure
storage regions.

The convenience `firm{}` path still owns default frame backing, but it now uses
an explicit aligned `owned_frame_storage` block instead of
`std::vector<std::byte>`. That keeps the default path as a bounded byte region
with a visible capacity, matching the borrowed frame-storage API more closely.
Default child-record, child-completion, and join-failure backing have the same
wrapper shape: `owned_firm_child_storage`,
`owned_firm_completion_storage`, and `owned_firm_join_storage` own bounded
arrays and then lend their borrowed views to the firm.

The current deed result state is inline in the deed handle and move-stable:
when the handle moves, the child record is retargeted to the new result slot.
That is still only the first result-slot shape, but the important semantic split
has landed. The firm owns settlement records. A deed owns or names the selected
result after evacuation.
The deed state also remembers the deck-assigned `task_id` of the child it
observes, so a deed can name the child without owning the child record. Its
common base now also carries the generic observation metadata: whether the deed
was contained, whether the result or failure was observed, and whether the
result was already taken. Typed result storage has also been named as a
`deed_result_slot<T>` helper, so the current inline storage path is explicit
and non-void deeds can now redirect evacuation into caller-provided storage via
`deed<T>::store_result_in(T&)` when `T` is assignable. That implements the
first `T`/`T *` result-target shape without yet moving deed records into
firm-local storage. Deeds can also borrow a typed
`deed_result_storage<T>` cell, which gives result evacuation uninitialized
external storage and therefore works for move-only, non-default-constructible
results without assigning into a preexisting `T`.
The generic metadata has also been named as
`deed_record_header`, separate from typed result slots, so the eventual
firm-local or borrowed deed record has a clear header shape. Firms now allocate
bounded issued-deed records from `firm_deed_storage_ref` /
`static_firm_deed_storage<N>` on each fork. Those records name which child task
was issued as a deed. They deliberately store task identity, not live child
record pointers; the live deed handle still owns or names the typed result slot
until a fuller firm-local deed lifetime lands.

Child final-suspend reporting also uses the firm record now. A forked promise
stores a raw pointer to its child record as its completion observer; it no
longer owns a type-erased completion callable just to notify the firm.
The child record also remembers the deck-assigned `task_id` returned when the
child is enqueued, so firm bookkeeping has begun naming forked work by durable
task identity instead of only by coroutine handle.
If deck registration fails after a child record has been constructed, `fork()`
destroys that record and leaves the firm with no phantom child to join.
That generic metadata now has a named `firm_child_record_header`, keeping firm
identity and completion-reporting state distinct from typed result handling.

Task cancellation bookkeeping has started moving the same way. Parent-stop
propagation and parked-wish cancellation now use concrete in-promise
`std::stop_callback` slots instead of heap-allocated, type-erased callback
boxes.

If [RFC 0002](rfc-0002-firm-frame-arenas.md) makes coroutine frames firm-local,
the records that describe those children should become firm-local as well.

## Proposal

A firm borrows or owns explicit bookkeeping storage:

```text
child records
deed records
join feed storage
cancellation waiters
settlement queues
debug names / labels
```

Forked children are recorded by `task_id`, with optional links to their result
storage and frame record. The child task frame remains in firm frame land. The
deck task registry records hot task state. The firm bookkeeping records the
structured relationship:

```text
firm spawned task_id
deed observes task_id
join waits for child completions
```

This matches the current model vocabulary in
[runtime.rkt](../../nxtrt/runtime.rkt): firms spawn tasks, issue deeds, and
deeds observe tasks.

## Deeds

A `deed<T>` should be an RAII handle naming a forked child and, optionally,
the storage where that child's result should be preserved after final suspend.
Today that handle already exposes the child's `task_id`; the remaining work is
to decide whether the deed record itself is inline in the handle, firm-local,
or a small borrowed record that the handle names.

If a deed remains alive, the child result can be moved into deed-owned or
deed-named storage at final suspend. If the deed is gone and no one will
observe the result, the task can report completion into firm join state and
destroy its frame after the firm's settlement rules allow it.

The current initial shape is a small result target in the child/deed record:

```cpp
template<typename T>
struct deed_result_slot {
    std::variant<
        std::monostate,
        T,
        T *,
        deed_result_storage<T> *,
        std::exception_ptr> storage;
};
```

That sketch is deliberately small. A deed can carry inline storage for the
result, assign into caller-provided live storage, or construct into a borrowed
typed `deed_result_storage<T>` cell. At child final suspend, the task
evacuates its result out of the coroutine frame into that slot if the deed is
still present. After evacuation, the frame can be destroyed according to firm
settlement rules without losing the selected result.

In the current implementation, result evacuation and frame reuse are separate
events. Evacuation moves or reports the typed result into the deed slot, then
destroys the coroutine frame. The first firm frame arena remains monotonic:
destroying the frame runs destructors and unregisters runtime state, but it
does not make those arena bytes available to a later child until the whole firm
settles or a future arena policy chooses explicit reuse.

This separates two ideas that are currently intertwined:

```text
join all children for structured settlement
observe selected child results via deeds
```

The firm always owns settlement. A deed only owns observation rights.

## Fork API Direction

Fork should prefer a task factory:

```cpp
fork(fn, args...) -> deed<T>
```

instead of only:

```cpp
fork(task<T>) -> deed<T>
```

This matters for two reasons.

First, task construction can happen inside the firm whose frame arena should
own the coroutine frame.

Second, it gives the API a natural place to avoid or make explicit coroutine
capture hazards. A capturing lambda whose `operator()` is itself a coroutine
can outlive its closure if the returned task escapes the lambda object. The
runtime API should guide code toward named coroutine helpers or explicit
argument passing.

Fork storage failure should initially be an exception with a firm diagnostic.
Forking is a synchronous attempt to allocate child/frame/bookkeeping land, so a
failed fork is not a pending asynchronous result. A later typed construction API
can wrap that exception if value-composition helpers want expected-like
behavior.

This RFC is only about the bookkeeping substrate. Higher-level value
composition over task factories belongs in
[RFC 0014: Idea Algebra](../new/rfc-0014-idea-algebra.md).

## Main Work As A Child

When a sibling task needs to stop the main work in a scope, the main work
should itself be a forked child owned by that firm. A firm body is not
automatically the same thing as one of the firm's child deeds.

That distinction matters for helpers such as `wait_any`, `with_timeout`, and
future channel pumps. The work that can be cancelled by a sibling must be in
the same child set the sibling can stop.

The current helper audit matches that rule. `wait_any`, `wait_any_range`,
`when_all`, and `when_all_range` build a policy firm and fork the candidate
tasks before joining. `with_timeout` similarly forks both the body task and the
timer task, so whichever one completes first can stop the sibling through the
firm child set. Plain `with_firm` remains different on purpose: its body is the
scope manager, not automatically one of the managed child tasks.

## Storage Shape

The first version can use fixed-capacity arrays over borrowed storage:

```text
firm_child_record children[N]
firm_deed_record deeds[M]
child_completion completions[K]
join_failure exceptions[J]
```

Later versions can use the extracted ring geometry from
[RFC 0007](rfc-0007-ring-geometry-extraction.md) for completion queues and
free lists.

Overflow is a real condition and should be reported as structured diagnostics:

- firm id/name;
- requested child or deed record;
- capacity and high-water mark;
- current task id;
- suggested storage class if available.

The current constructor policy keeps the common path compact. Convenience firm
constructors derive deed-record, completion, and join-failure capacities from
the child-record capacity. When only deed-record capacity needs to differ, a
firm can borrow child and deed storage directly. When several capacities need
to differ, the explicit path is the bookkeeping bundle
`firm_bookkeeping_storage_ref` /
`static_firm_bookkeeping_storage<Children, JoinFailures, Completions, Deeds>`.
The full-land aggregate
`static_firm_storage<FrameBytes, Children, JoinFailures, Completions, Deeds>`
still exists for callers that want to grant frame and bookkeeping storage as
one visible region bundle.

## Invariants

A child task belongs to exactly one firm.

A deed observes a child spawned by the same firm that issued the deed.

Destroying or dropping a deed does not detach the child from structured
settlement. It only drops the caller's right to recover that child's result.

Firm bookkeeping storage cannot be reclaimed until the firm has settled all
children and no live deed can name the records being reclaimed.

## Relationship To Other RFCs

[RFC 0002](rfc-0002-firm-frame-arenas.md) provides frame land.

[RFC 0003](rfc-0003-deck-task-registry.md) provides durable task IDs.

[RFC 0006](rfc-0006-join-as-a-completion-feed.md) describes the join
side of the same bookkeeping as a feed of child completions.

[RFC 0007](rfc-0007-ring-geometry-extraction.md) provides reusable bounded
storage machinery for queues and free lists.

## Open Questions

- Should firms provide typed result-storage pools that lend
  `deed_result_storage<T>` cells, or should callers continue granting those
  cells explicitly?
- Should `firm_storage_ref` be rebuilt internally around
  `firm_bookkeeping_storage_ref`, or is it useful for the full-land bundle to
  keep flat named fields?
- Should later frame arenas reuse destroyed child frames before firm
  settlement, or should the default remain monotonic until the firm settles?
- Which future helpers or channel pumps need their main body turned into an
  explicit child?

## References

- [RFC 0002: Firm Frame Arenas](rfc-0002-firm-frame-arenas.md)
- [RFC 0006: Join as a Completion Feed](rfc-0006-join-as-a-completion-feed.md)
- [RFC 0014: Idea Algebra](../new/rfc-0014-idea-algebra.md)
- [Runtime Overview / Firms and Deeds](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [Behavioral threads as occurrent structure](../../docs/rt-occurrents.md)
- [task.hpp](../../src/nxtrt/task.hpp)
- [runtime.rkt](../../nxtrt/runtime.rkt)
