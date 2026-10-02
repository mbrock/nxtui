# RFC 0006: Join as a Completion Feed {#rfc_join_completion_feed}

Status: new

## Implementation status

This remains a proposal, not the current join algorithm. The nursery
simplification removed the bounded completion ledger because join did not
consume it. Current join traverses child records; final-suspend notification
still drives cancellation policies directly. Any future completion feed should
replace that traversal with an actual consumer, rather than add a parallel
ledger or impose a fixed child-admission bound.

The implemented [bounded pool](../../docs/rt-pool.md) is a separate
completion-order feed of homogeneous idea results, not a replacement firm
join. It owns tasks directly, without firm child records or deeds. Its slots
remain occupied until output consumption (or close/discard), even though a
completed task's frame can be released earlier. Unhandled job errors fail the
pool when the consumer encounters them, not through a completion-time sibling
cancellation policy. See the [concurrency direction](../../docs/rt-concurrency-direction.md)
for the distinction between a growable nursery, bounded circulation, and
proposed static teams.

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

The current promise has a generic completion observer hook, shared by firm
child records and pool slots:

```cpp
promise.observe_completion_of(observer);
```

That notification is a possible place to publish a completion item. This RFC
proposes making firm join consume those items; the observer alone does not
implement that feed.

## Proposal

A firm would own a completion feed, with storage following admitted children
rather than imposing a fixed bound on the growable nursery:

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

## Historical Bounded-Feed Alternatives

The original proposal considered a bounded join feed. The alternatives below
remain design questions for a bounded owner, not the current firm's capacity
contract. The bounded ledger was removed; it must not be reintroduced without
a consumer and a demonstrated need.

If final suspend cannot publish a completion item because the firm completion
feed is full, publication needs an explicit protocol. Parking final suspend is
one speculative alternative, not a supported operation today:

- make completion feed capacity at least the maximum child count;
- let final suspend park until the join feed has space;
- reserve one completion slot per child record;
- treat completion overflow as a firm storage error.

The pool instead reserves a slot before reading and invoking an idea. Final
suspend only queues readiness and wakes the consumer through the deck; it
does not wait for result storage or destroy its own frame. Consumption returns
admission credit. This keeps bounded backpressure at admission and consumption,
without giving the ordinary firm a fixed child capacity.

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

[RFC 0005](../cur/rfc-0005-firm-bookkeeping-without-heap-vectors.md) defines the
history of firm bookkeeping and the removal of its bounded completion ledger.

[RFC 0007](../cur/rfc-0007-ring-geometry-extraction.md) can provide the bounded queue
geometry for the completion feed.

[RFC 0008](rfc-0008-pushfeed-channels-and-removing-bell-wire.md) proposes
deck-local producer/consumer machinery that this completion feed could
resemble; the generic channel is not implemented.

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

- [RFC 0005: Firm Bookkeeping without Heap Vectors](../cur/rfc-0005-firm-bookkeeping-without-heap-vectors.md)
- [RFC 0007: Ring Geometry Extraction](../cur/rfc-0007-ring-geometry-extraction.md)
- [RFC 0008: Pushfeed Channels and Removing Bell/Wire](rfc-0008-pushfeed-channels-and-removing-bell-wire.md)
- [RFC 0014: Idea Algebra](rfc-0014-idea-algebra.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [Behavioral threads as occurrent structure](../../docs/rt-occurrents.md)
- [task.hpp](../../src/nxtrt/task.hpp)
- [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp)
- [runtime.rkt](../../nxtrt/runtime.rkt)
