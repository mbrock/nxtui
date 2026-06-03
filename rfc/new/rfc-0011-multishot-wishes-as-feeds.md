# RFC 0011: Multishot Wishes as Feeds {#rfc_multishot_wishes_as_feeds}

Status: new

## Summary

One-shot wishes produce one completion. Multishot wishes produce feeds.

Examples:

```text
accept_multishot(listener) -> feed<accepted_socket>
recv_multishot(socket)    -> feed<byte_chunk>
watch(fd)                 -> feed<event>
```

A multishot urge is not settled by one backend completion. It is a live
producer of feed items until a final completion, error, cancellation, or scope
settlement.

## Motivation

The current wish vocabulary is one-shot. `op::accept` returns one file
descriptor. `op::recv_some` receives into one buffer. The wand's exec lifecycle
settles a task after one operation CQE.

io_uring and other backends can produce more than one completion for a single
submitted operation. Linux exposes this through flags such as
`IORING_CQE_F_MORE` for multishot operations. A one-shot `urge<T>` is not the
right abstraction for those completions.

The runtime already has the right higher-level vocabulary: feeds. A multishot
operation is a backend producer of feed items.

## Proposal

Represent multishot operations as feeds with explicit storage:

```cpp
auto accepted = accept_multishot(listener, accepted_storage);
auto chunks = recv_multishot(socket, buffer_group, mark_storage);
```

The returned object is not a single `task<T>`. It is a feed-like source whose
cold path is driven by a live backend operation.

The wand owns the backend realization. The feed owns visible items and
backpressure policy. The firm owns the scope and, usually, the buffer territory.

## Lifecycle

A multishot realization has at least these phases:

```text
prepared
parked/live
producing
finishing
settled
retired
```

Unlike a one-shot wish, producing one item does not settle the realization. It
publishes one feed item and remains live if the backend says more completions
may arrive.

The final backend signal closes the feed or records an error. Cancelling the
firm cancels the multishot realization and wakes any feed consumers.

## Feed Items

Different multishot wishes produce different item types:

```cpp
struct accepted_socket {
    int fd;
    sockaddr_storage peer;
};

struct recv_chunk {
    byte_loan loan;
    std::span<const std::byte> bytes;
    recv_flags flags;
};

struct watch_event {
    event_kind kind;
    payload payload;
};
```

For provided-buffer recv, the item should carry a buffer loan rather than an
owned byte vector. This ties the RFC to
[RFC 0010](rfc-0010-firm-buffer-groups-and-io-land.md).

## Backpressure

There are two buffers to consider:

- backend completion capacity;
- runtime feed capacity.

If the feed is full when a backend completion arrives, the runtime must either
have reserved enough feed capacity, apply backpressure to the backend when the
platform allows it, or treat overflow as a runtime storage error.

For provided-buffer receive, returning buffers to the kernel is also part of
backpressure. Holding too many borrowed chunks in userland can starve the
buffer group.

## Relationship To exec Records

[RFC 0004](rfc-0004-wand-completion-routing.md) removes the universal exec hub
from ordinary one-shot wishes. Multishot wishes are the main case where a
wand-owned realization record still makes sense.

A multishot exec can own:

- backend operation identity;
- feed producer state;
- cancellation and final-drain state;
- links to buffer groups;
- the task or feed waiters to wake on new items.

The difference is that the exec now represents a live producer, not a generic
one-shot parking record.

## Invariants

Every non-final backend completion either publishes one feed item, records an
error, or is explicitly discarded by a documented policy.

A final completion closes or errors the feed exactly once.

Cancelling the owning firm eventually cancels the live backend producer and
wakes all consumers.

Borrowed buffer items keep their backing territory live until the item is
released or consumed according to the feed's rules.

## Relationship To Other RFCs

[RFC 0006](rfc-0006-join-as-a-completion-feed.md) treats child task completion
as a feed. This RFC applies the same reading to platform completions.

[RFC 0008](rfc-0008-pushfeed-channels-and-removing-bell-wire.md) provides the
deck-local feed/waiter machinery that multishot producers can use.

[RFC 0009](rfc-0009-wishes-urges-and-provided-buffers.md) and
[RFC 0010](rfc-0010-firm-buffer-groups-and-io-land.md) provide the buffer
vocabulary for multishot receive.

## Open Questions

- Which multishot operations should be modeled first: accept, recv, poll/watch,
  or child-process/event streams?
- Is a multishot feed started immediately, or only when first consumed?
- How does a multishot feed expose errors after some items have already been
  delivered?
- Can one multishot producer have multiple consumers, or is it single-receiver
  like `wire`?
- How should final CQE flags and partial errors be represented portably?

## References

- [RFC 0004: Wand Completion Routing without exec Hub](rfc-0004-wand-completion-routing.md)
- [RFC 0006: Join as a Completion Feed](rfc-0006-join-as-a-completion-feed.md)
- [RFC 0010: Firm Buffer Groups and I/O Land](rfc-0010-firm-buffer-groups-and-io-land.md)
- [Runtime Overview / Wands](../../docs/rt-overview.md)
- [wish_ops.hpp](../../src/nxtrt/wish_ops.hpp)
- [wand.hpp](../../src/nxtrt/wand.hpp)
- [wand/uring.hpp](../../src/nxtrt/wand/uring.hpp)
- [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp)
