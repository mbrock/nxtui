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

That is still provisional: deed result states are currently move-stable
objects owned uniquely by deed handles. But the important semantic split has
landed. The firm owns settlement records. A deed owns or names the selected
result after evacuation.

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

If a deed remains alive, the child result can be moved into deed-owned or
deed-named storage at final suspend. If the deed is gone and no one will
observe the result, the task can report completion into firm join state and
destroy its frame after the firm's settlement rules allow it.

The likely shape is a small result target in the child/deed record:

```cpp
template<typename T>
struct deed_result_slot {
    std::variant<std::monostate, T, T *> storage;
};
```

That sketch is not the API, but it captures the ownership choice. A deed can
carry inline storage for the result, or it can name caller/firm-provided
storage. At child final suspend, the task evacuates its result out of the
coroutine frame into that slot if the deed is still present. After evacuation,
the frame can be destroyed according to firm settlement rules without losing
the selected result.

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

## Storage Shape

The first version can use fixed-capacity arrays over borrowed storage:

```text
firm_child_record children[N]
firm_deed_record deeds[M]
child_completion completions[K]
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

[RFC 0006](../new/rfc-0006-join-as-a-completion-feed.md) describes the join
side of the same bookkeeping as a feed of child completions.

[RFC 0007](rfc-0007-ring-geometry-extraction.md) provides reusable bounded
storage machinery for queues and free lists.

## Open Questions

- What exact result-slot type lets final suspend evacuate a result into a
  deed without the temporary move-stable allocation used by the transition
  code?
- What are the default capacities for child records, deeds, and completions?
- How should child result destruction interact with frame reuse?
- Which current helpers need their main body turned into an explicit child?

## References

- [RFC 0002: Firm Frame Arenas](rfc-0002-firm-frame-arenas.md)
- [RFC 0006: Join as a Completion Feed](../new/rfc-0006-join-as-a-completion-feed.md)
- [RFC 0014: Idea Algebra](../new/rfc-0014-idea-algebra.md)
- [Runtime Overview / Firms and Deeds](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [Behavioral threads as occurrent structure](../../docs/rt-occurrents.md)
- [task.hpp](../../src/nxtrt/task.hpp)
- [runtime.rkt](../../nxtrt/runtime.rkt)
