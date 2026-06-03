# RFC 0008: Pushfeed Channels and Removing Bell/Wire {#rfc_pushfeed_channels}

Status: new

## Summary

Replace fd-backed internal synchronization with deck-local feed and sink
coordination.

The current `wire<T>` is already shaped like a channel: a receive endpoint is a
`feed<T>`, and a transmit endpoint is a `sink<T>`. But when a producer or
consumer must wait, `wire` uses `bell`, and `bell` wakes through platform fd
readiness and the active wand's `poll` wish.

For internal runtime synchronization, that trip through eventfd, poll, and the
wand should disappear. A channel should park tasks directly in deck-local
single-wait slots and wake them by `task_id`.

## Current Shape

In [wire.hpp](../../src/nxtrt/wire.hpp):

- `wire_rx<T>` derives from `feed<T>`;
- `wire_tx<T>` derives from `sink<T>`;
- receive storage belongs to the receiver;
- zero capacity uses a rendezvous slot;
- `next()` and `send()` return `hope`, using a ready path when possible.

In [bell.hpp](../../src/nxtrt/bell.hpp):

- `bell` is a manual-reset readiness object backed by `eventfd` on Linux or a
  pipe fallback;
- waiting on a bell awaits `op::poll`;
- ringing a bell writes to the fd so the wand can observe readiness.

That is appropriate for crossing a platform event-loop boundary. It is too
expensive and too indirect for waking sibling tasks in the same deck.

## Proposal

Introduce a deck-local push feed as a `feed<T>` subclass:

```text
rx = pushfeed<T> : feed<T>, over explicit storage
tx = sink<T> whose cold drain pushes into rx
```

The receive endpoint owns the buffer. The transmit endpoint has no independent
buffer unless a higher layer chooses to add one. When the sender cannot push,
it parks on receiver capacity. When the receiver cannot take, it parks on
receiver data.

The first version should use single-wait semantics:

```text
at most one cold receive wait
at most one cold send/capacity wait
```

Those slots contain `task_id`, so this RFC depends on
[RFC 0003](rfc-0003-deck-task-registry.md). Multiple waiters, round-robin
selection, broadcast, and fan-out can be built later as explicit layers over
the single-wait core.

## Semantics

The hot paths stay synchronous:

- `try_next()` succeeds if data is already present;
- `next()` returns `hope::ready(value)` on a data hit;
- `try_send(value)` succeeds if capacity or rendezvous demand exists;
- `send(value)` returns `hope::ready(true)` on a capacity hit.

Cold paths park the current task:

```text
consumer waits on data
producer waits on capacity
close wakes both sides
cancel clears the matching wait slot
```

No fd is created. No `op::poll` wish is issued. No wand is required.

## Rendezvous

A zero-capacity channel is rendezvous. Rendezvous should not be scalar-only.
The channel layer should understand chunks:

```text
producer chunks -> waiting stream consumer
```

If a producer is streaming into a channel and the consumer is already waiting
with a downstream sink, the values may flow directly into that sink. If the
downstream sink suspends, the producer suspends too unless a buffer or pump
task boundary is introduced.

This preserves backpressure and avoids creating hidden queues.

## Bells

A bell becomes one of:

```text
pushfeed<std::monostate>
```

or a lighter readiness primitive using the same deck-local wait-slot machinery.

It may also disappear entirely from the runtime core if every current use can
be expressed as a pushfeed or a direct deck-local readiness slot. The fd-backed
form should remain only if a host/event-loop boundary really needs a pollable
object.

## Relationship To Feeds

The receive endpoint should still feel like a feed. It owns visible buffered
values and exposes hot-path borrowing/consuming operations where those are
valid.

The channel-specific addition is pushback from a producer sink. A normal
`feed<T>` asks its source to produce more values. A `pushfeed<T>` is filled by
outside producers that are themselves tasks.

This is another instance of the holder pattern from
[rt-holding](../../docs/rt-holding.md): the channel holds values and parked
tasks; its release rule is data/capacity availability.

## Invariants

A task parked in a channel wait slot is not ready on the deck.

Closing a channel wakes the parked producer and consumer, if present.

Dropping a transmit endpoint does not destroy buffered receive values. Dropping
the receive endpoint closes the channel and wakes producers.

Rendezvous transfer either completes exactly once or is cancelled exactly once.

## Relationship To Other RFCs

[RFC 0003](rfc-0003-deck-task-registry.md) provides task IDs for wait slots.

[RFC 0007](rfc-0007-ring-geometry-extraction.md) provides the buffer geometry.

[RFC 0006](rfc-0006-join-as-a-completion-feed.md) can use the same feed
machinery for child completions.

[RFC 0011](rfc-0011-multishot-wishes-as-feeds.md) uses similar producer
semantics for platform-backed feeds.

## Open Questions

- Which `feed<T>` protected hooks does `pushfeed<T>` need so subclassing stays
  clean?
- Is one producer wait slot and one consumer wait slot enough for every first
  use?
- How should cancellation clear a task from a wait slot efficiently?
- How much direct chunk transfer should the first rendezvous implementation
  support?
- Is fd-backed `bell` still needed after pushfeed replaces wire?

## References

- [RFC 0003: Deck Task Registry and Task IDs](rfc-0003-deck-task-registry.md)
- [RFC 0007: Ring Geometry Extraction](rfc-0007-ring-geometry-extraction.md)
- [Runtime Overview](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [wire.hpp](../../src/nxtrt/wire.hpp)
- [bell.hpp](../../src/nxtrt/bell.hpp)
- [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp)
