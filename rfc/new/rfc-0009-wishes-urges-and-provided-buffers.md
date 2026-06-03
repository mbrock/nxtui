# RFC 0009: Wishes, Urges, and Provided Buffers {#rfc_wishes_provided_buffers}

Status: new

## Summary

Split pointer-carrying I/O wishes from storage-selecting I/O wishes.

The current `read_some` and `recv_some` wishes carry caller-owned spans. That
is still useful and should remain available as `read_into` and `recv_into`.
But the default streaming receive/read path should allow the runtime to choose
buffer territory at realization time.

In short:

```cpp
read_into(fd, span)        // explicit caller-owned destination
recv_into(fd, span)

read_some(fd, max)         // runtime-selected buffer
recv_some(fd, max)
```

A wish names the desired interaction. An urge is the realized operation after
the wand combines that wish with the current task, firm, and platform state.

## Motivation

The runtime notes describe a wish as a platform-neutral desire and a wand as
the thing that grants it. See [rt-holding / wish](../../docs/rt-holding.md) and
[Runtime Overview / Wishes](../../docs/rt-overview.md).

The current API partly breaks that abstraction for byte input. A wish such as
`op::recv_some` already contains a concrete destination span:

```cpp
struct recv_some : wish<std::size_t, "recv"> {
    int fd;
    std::span<std::byte> buffer;
    int flags;
};
```

That means buffer territory has been selected before the wand sees the current
firm, current task, backend capabilities, or provided-buffer groups.

For ordinary pull-shaped reads into a caller's buffer, this is fine. For
streaming I/O, it hides the more interesting operation: "receive some bytes,
using appropriate runtime-owned byte land, and give me a borrowed chunk."

## Proposal

Keep explicit destination wishes:

```cpp
op::read_into { fd, buffer, offset }
op::recv_into { fd, buffer, flags }
```

Add storage-selecting wishes:

```cpp
op::read_some { fd, max, offset }
op::recv_some { fd, max, flags }
```

The storage-selecting wish contains no buffer pointer, firm id, task id, SQE
details, or provided-buffer id. At realization time, the wand combines:

- the wish;
- current task id and await slot;
- current firm;
- firm buffer groups visible to the wand;
- platform backend;
- operation flags and limits.

The result is an urge with concrete backend state.

## Provided Buffers

Provided buffers should be a native runtime concept, not merely an io_uring
implementation detail. The wand base layer should understand semantic buffer
groups and buffer loans even when a backend realizes them with ordinary memory
instead of kernel-provided-buffer machinery.

On Linux, `recv_some(fd, max)` can map naturally to io_uring provided buffers:

```text
SQE names bgid
kernel selects bid
CQE returns bid + len
task receives borrowed chunk view
```

The returned value should name a borrowed region, not copy bytes into the
coroutine frame:

```cpp
struct byte_loan {
    buffer_group_id group;
    buffer_id id;
    std::span<const std::byte> bytes;
};
```

The exact type is open. The important property is that release is explicit and
the backing memory belongs to a firm-owned buffer group, not to a local
temporary span hidden inside a wish.

## Wish vs Urge

This RFC sharpens the wish/urge split:

```text
wish = closed operation recipe
urge = realized operation in a concrete runtime context
```

A wish should be copyable or movable as a description of what is wanted. It
should not contain backend-selected territory.

An urge may contain task routing, await-slot generation, provided-buffer group
ids, cancellation details, SQE encodings, and completion-sink routing. It is
the platform realization of the wish. Typed result storage stays with the
awaiter/promise side, as described in
[RFC 0004](rfc-0004-wand-completion-routing.md).

This also aligns with [RFC 0004](rfc-0004-wand-completion-routing.md): a
one-shot urge can often be represented by the current task's await slot instead
of a wand exec.

## Feed Integration

Byte feeds should prefer storage-selecting wishes where possible.

A `socket_source` backed by provided buffers does not need to ask its own
`feed<std::byte>` storage to receive bytes directly. It can receive a borrowed
chunk from a buffer group, expose that chunk through feed/reel operations, and
release it when the feed discards the consumed prefix.

This will require careful lifetime rules. The default feed validity rule still
applies: borrowed chunk views are invalidated by operations that refill,
stream, discard, or consume the source.

## Relationship To Firm I/O Land

[RFC 0010](rfc-0010-firm-buffer-groups-and-io-land.md) answers where the
runtime-selected buffers come from. This RFC only changes the operation
vocabulary so a wish can ask for runtime-selected storage.

## Invariants

Pointer-carrying wishes must not outlive the caller-owned destination they
name.

Storage-selecting wishes contain no raw destination pointer.

A borrowed completion chunk has a visible owner and release rule.

The wand must not hand a task a buffer that the current firm is not permitted
to use.

## Open Questions

- What is the fallback realization of `read_some` and `recv_some` on platforms
  without kernel-provided buffers?
- Can file reads use the same storage-selecting vocabulary, or is this mostly
  useful for sockets and pipes?
- Does a feed own a borrowed completion chunk until discard, or does it develop
  that chunk into its ordinary ring storage immediately?
- How should buffer loans interact with reels and borrowed frame projections?
- How should errors report buffer group and buffer id context?

## References

- [RFC 0004: Wand Completion Routing without exec Hub](rfc-0004-wand-completion-routing.md)
- [RFC 0010: Firm Buffer Groups and I/O Land](rfc-0010-firm-buffer-groups-and-io-land.md)
- [RFC 0001: Reels](rfc-0001-reels.md)
- [Runtime Overview / Wishes](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [wish_ops.hpp](../../src/nxtrt/wish_ops.hpp)
- [wand.hpp](../../src/nxtrt/wand.hpp)
- [wand/uring.hpp](../../src/nxtrt/wand/uring.hpp)
- [buffers.hpp](../../src/nxtrt/buffers.hpp)
