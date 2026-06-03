# RFC 0006: Join as a Completion Feed {#rfc_join_completion_feed}

Status: new

## Summary

Firm join should be modeled as a feed of child completions.

When a forked child reaches final suspend, it publishes a completion record
into its firm's join feed. Joining means draining that feed until every child
owned by the firm has settled.

This makes child completion another stream event in the runtime, rather than a
special side table that only `join()` understands.

## Motivation

The runtime already treats many forms of held work as buffers or feeds. The
deck holds ready tasks. The wand holds wishes. A bytefeed holds source stock.
See [rt-holding](../../docs/rt-holding.md) for the full holder reading.

Child completion has the same shape:

```text
producer: final suspend of a child task
buffer: firm completion storage
consumer: join policy
item: child_completion
```

The current `firm` has a completion callback hook:

```cpp
promise.completion_callback = ...
```

That callback is exactly the place where a child becomes a completion item.
This RFC makes that hidden callback path into an explicit feed.

## Proposal

A firm owns a bounded completion feed:

```cpp
struct child_completion {
    task_id child;
    completion_kind kind;
    exception_or_status status;
};
```

At final suspend, a forked child first evacuates its typed result into its deed
result slot, if a deed still names one. Then it publishes one
`child_completion` into the firm's join feed. The completion item stays small:
it carries identity and status, not the selected typed result itself.

Joining drains completions:

```cpp
while (firm.has_unsettled_children()) {
    auto completion = co_await firm.completions.next();
    firm.mark_settled(completion.child);
    policy.observe(completion);
}
```

The actual API does not need to expose the raw feed immediately. The important
implementation move is that join becomes a consumer of completion items, with
the same backpressure and storage questions as other feeds. This is closely
related to [RFC 0008](rfc-0008-pushfeed-channels-and-removing-bell-wire.md):
a firm completion feed is a push feed whose producer is child final suspend.

## Policy Space

Once child completion is a feed, several join policies become ordinary
consumers:

- join all children and throw the first error;
- join all children and collect all errors;
- fail fast by stopping siblings when the first child fails;
- stream child results in completion order;
- wait for any child and leave the firm responsible for stopping or draining
  the rest;
- attach observers for tracing or UI progress.

This is also the natural base for `when_all`, `wait_any`, `with_timeout`, and
game-style coordination helpers.

## Backpressure

A bounded join feed can fill. That is not merely a nuisance; it is a useful
design pressure.

If final suspend cannot publish a completion item because the firm completion
feed is full, the child cannot fully settle yet. It is reasonable for that path
to suspend or otherwise cooperate with the scheduler; this is ordinary
backpressure, not a bug. The runtime must decide one of:

- make completion feed capacity at least the maximum child count;
- let final suspend park until the join feed has space;
- reserve one completion slot per child record;
- treat completion overflow as a firm storage error.

The first implementation should probably reserve enough completion storage for
the child capacity. Later versions can use a true feed if result streaming
needs finer backpressure.

This also hints at a larger rule beyond this RFC: time should become as
explicitly rationed as space. A suspension is not free just because it has no
bytes attached. Future task-composition APIs should make unbounded waiting
visible, likely by composing ideas with timeout or budget-bearing forms.

## Invariants

Every forked child publishes exactly one completion item.

A firm reaches joined state only after it has observed completion for every
spawned child.

A deed may observe a result, but deed observation does not replace the child's
completion item. The deed is the result evacuation target; settlement belongs
to the firm.

Completion publication happens at or immediately after the child's final
suspend boundary. In the occurrent vocabulary of
[rt-occurrents](../../docs/rt-occurrents.md), final suspend is the boundary
where the child history becomes available to the parent scope.

## Relationship To Other RFCs

[RFC 0005](rfc-0005-firm-bookkeeping-without-heap-vectors.md) defines the
firm-local storage where child and completion records live.

[RFC 0007](rfc-0007-ring-geometry-extraction.md) can provide the bounded queue
geometry for the completion feed.

[RFC 0008](rfc-0008-pushfeed-channels-and-removing-bell-wire.md) provides the
deck-local producer/consumer machinery that this completion feed should
resemble.

[RFC 0011](rfc-0011-multishot-wishes-as-feeds.md) applies the same feed reading
to platform operations that produce more than one completion.

[RFC 0014](rfc-0014-idea-algebra.md) sketches the value-composition layer above
firm joins and child deeds.

## Open Questions

- Should the join feed be publicly visible, or only an internal implementation
  vocabulary?
- Does a child completion item include exception status, or does it only carry
  `task_id` and let the deed/task table provide status?
- Can final suspend ever suspend to wait for completion-feed capacity, or must
  capacity be reserved ahead of time?
- How do completion feeds interact with fail-fast cancellation?
- How should future time budgets or deadlines appear in join policies without
  making every call site noisy?

## References

- [RFC 0005: Firm Bookkeeping without Heap Vectors](rfc-0005-firm-bookkeeping-without-heap-vectors.md)
- [RFC 0007: Ring Geometry Extraction](rfc-0007-ring-geometry-extraction.md)
- [RFC 0008: Pushfeed Channels and Removing Bell/Wire](rfc-0008-pushfeed-channels-and-removing-bell-wire.md)
- [RFC 0014: Idea Algebra](rfc-0014-idea-algebra.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [Behavioral threads as occurrent structure](../../docs/rt-occurrents.md)
- [task.hpp](../../src/nxtrt/task.hpp)
- [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp)
- [runtime.rkt](../../nxtrt/runtime.rkt)
