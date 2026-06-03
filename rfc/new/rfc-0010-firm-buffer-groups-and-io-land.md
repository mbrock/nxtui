# RFC 0010: Firm Buffer Groups and I/O Land {#rfc_firm_buffer_groups}

Status: new

## Summary

A firm may own or be granted I/O buffer territory in addition to coroutine
frame territory.

The wand owns backend machinery: io_uring, kqueue, epoll, submission policy,
and completion pumping. The firm owns the buffer groups, quotas, and regions
that its wishes may use, even when a wand registers those buffers with a
platform backend.

This mirrors the split from [RFC 0002](../cur/rfc-0002-firm-frame-arenas.md) and
[RFC 0003](../cur/rfc-0003-deck-task-registry.md):

```text
deck owns task registry
firm owns task frames
wand owns backend machinery
firm owns I/O buffer groups
```

## Motivation

[RFC 0000](rfc-0000-prolegomena.md) says buffers are territories. The layer
that owns the buffer pays the holding cost, while other layers borrow the
affordance.

The current byte feeds make buffer ownership visible at the feed layer:

```cpp
socket_source source{fd, buffer};
```

That is still the right shape for many readers. But storage-selecting wishes
from [RFC 0009](rfc-0009-wishes-urges-and-provided-buffers.md) need a visible
place to choose from. If `recv_some(fd, max)` does not carry a span, the
runtime must know which byte land the task is allowed to use.

The firm is the natural scope for that land. It already bounds child work. It
should also bound the I/O buffers consumed by that work.

## Proposal

Add firm-owned I/O buffer groups:

```cpp
struct firm_io_region {
    std::span<std::byte> backing;
    buffer_group_id group;
    std::size_t buffer_size;
    std::size_t buffer_count;
};
```

The exact API is open. The important concepts are:

- a firm owns one or more byte regions or buffer groups;
- each group has capacity and ownership rules;
- a wand can realize storage-selecting wishes against those groups;
- completions return borrowed chunks tied to a group and buffer id;
- release returns the territory to the group;
- firm cancellation or destruction cancels, drains, and destroys its groups.

For Linux io_uring, a firm buffer group may map to a provided-buffer `bgid`.
The backing memory can come from:

- a firm-owned static or dynamic arena;
- a wand-wide pool carved into firm-local quotas;
- an outer borrowed region granted to the firm;
- registered fixed buffers where the platform supports them.

## Ownership

The wand may register buffers with the kernel, but registration is not the same
as semantic ownership. Registration is backend machinery. The right to consume
and hold the bytes belongs to the firm.

This distinction keeps cancellation and structured concurrency legible. When a
firm stops, any outstanding buffer loans issued to that firm's tasks must be
completed, cancelled, or reclaimed according to firm settlement rules.

A buffer group is also an async resource in the sense of
[RFC 0015: Async RAII Resources](rfc-0015-async-raii-resources.md): it may need
backend setup, it emits loans while alive, and it has asynchronous teardown
work during cancellation or normal destruction.

## Buffer Loans

A storage-selecting wish returns a loan:

```text
group id
buffer id
byte extent
release rule
debug owner
```

The loan may be consumed by a feed, projected by a reel, developed into an
owned value, or streamed into a sink. Borrowed views are valid only until the
operation that releases or mutates the backing buffer.

The release rule must be explicit. For provided buffers, release usually means
returning the buffer id to the ring. For ordinary firm memory, release may mean
marking a region free in a firm-local allocator.

## Relationship To Feeds

There are two possible integrations:

1. A feed copies or moves received bytes into its existing ring storage, then
   releases the loan immediately.
2. A feed temporarily exposes the loan as visible stock and releases it when
   the consumed prefix advances past it.

The second path is more powerful and fits [RFC 0001: Reels](rfc-0001-reels.md)
better, but it requires segmented stock and stricter lifetime tracking.

The first implementation can choose the simpler path while keeping the loan
type and ownership visible.

## Invariants

A task may only receive buffer loans from groups granted to its current firm.

A buffer id is either free, loaned, submitted to the backend, or being
reclaimed. It is never silently shared by two unrelated tasks.

Stopping a firm eventually cancels, drains, or releases every outstanding
buffer loan issued to that firm.

The wand may own registration and backend handles, but it may not hide the
holding cost of byte land from the firm or runtime diagnostics.

## Relationship To Other RFCs

[RFC 0009](rfc-0009-wishes-urges-and-provided-buffers.md) introduces the
storage-selecting wishes that need this territory.

[RFC 0007](../cur/rfc-0007-ring-geometry-extraction.md) provides reusable geometry for
buffer-group free lists and visible chunks.

[RFC 0011](rfc-0011-multishot-wishes-as-feeds.md) uses buffer groups heavily
for multishot receive feeds.

[RFC 0015](rfc-0015-async-raii-resources.md) describes buffer groups as scoped
async resources owned by firms.

## Open Questions

- How should groups be identified across backend APIs?
- What is the minimum useful buffer-loan type?
- Can a single loan be partially consumed and returned with a suffix still
  live, or must release be whole-buffer?
- How should diagnostics report buffer pressure by firm and by backend?
- What is the smallest async teardown protocol for cancelling outstanding
  loans and unregistering backend buffers?

## References

- [RFC 0000: Prolegomena to NXT System Theory](rfc-0000-prolegomena.md)
- [RFC 0002: Firm Frame Arenas](../cur/rfc-0002-firm-frame-arenas.md)
- [RFC 0009: Wishes, Urges, and Provided Buffers](rfc-0009-wishes-urges-and-provided-buffers.md)
- [RFC 0015: Async RAII Resources](rfc-0015-async-raii-resources.md)
- [RFC 0001: Reels](rfc-0001-reels.md)
- [Runtime Overview / Byte streams and protocol helpers](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [buffer-core.hpp](../../src/nxtrt/buffer-core.hpp)
- [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp)
- [wand/uring.hpp](../../src/nxtrt/wand/uring.hpp)
