# RFC 0007: Ring Geometry Extraction {#rfc_ring_geometry_extraction}

Status: new

## Summary

Extract the pure ring-buffer machinery from `feed<T>` and `sink<T>` into a
lower-level non-coroutine component.

The extracted layer should know about borrowed storage, constructed and
unconstructed regions, two-span chunk views, capacity, rebase, and overflow. It
should not know about virtual functions, tasks, hopes, wands, feeds, sinks, or
protocol parsing.

## Motivation

The current buffer stack already contains the geometry this RFC wants to make
reusable:

- [buffer-core.hpp](../../src/nxtrt/buffer-core.hpp) defines
  `buffer_chunks<T>`, `ring_chunks()`, `ring_write_index()`, and
  `ring_unused_capacity()`.
- [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp) uses `seek_`,
  `size_`, `capacity_`, raw storage, and constructed-value lifetime management
  in both `sink<T>` and `feed<T>`.
- [buffers.hpp](../../src/nxtrt/buffers.hpp) builds byte feeds, byte sinks,
  `chop_view`, and `reel` on top of that value-buffer layer.

The current helpers are useful but not yet a component. Reusing the same
geometry for deck ready queues, firm completion feeds, channel buffers,
provided-buffer bookkeeping, and raw uring ring views currently means either
copying patterns or depending on the coroutine feed/sink layer.

## Proposal

Introduce a pure ring geometry type. The exact name is open, but the shape is:

```cpp
template<typename T>
class ring_region {
public:
    value_chunks<T> constructed();
    junk<T> unconstructed_capacity();

    std::size_t size() const;
    std::size_t capacity() const;
    std::size_t contiguous_capacity() const;

    void advance_constructed(std::size_t n);
    void destroy_prefix(std::size_t n);
    void consume_prefix_without_destroying(std::size_t n);
    void rebase(std::size_t preserve, std::size_t capacity);
};
```

The layer should support both trivially copyable and non-trivial `T`:

- for bytes and POD values, it can expose writable byte spans and use memmove
  for rebase;
- for non-trivial values, it must construct, move, and destroy objects
  correctly.

The component should report capacity failures directly and with enough context
for higher layers to add firm/deck/wand names.

## Users

The first users should be:

- `feed<T>` and `sink<T>`;
- deck ready queues once [RFC 0003](rfc-0003-deck-task-registry.md) moves them
  to `task_id`;
- firm join feeds from [RFC 0006](rfc-0006-join-as-a-completion-feed.md);
- pushfeed channels from [RFC 0008](rfc-0008-pushfeed-channels-and-removing-bell-wire.md);
- provided-buffer rings from
  [RFC 0010](rfc-0010-firm-buffer-groups-and-io-land.md);
- mark buffers for optimized reels, if [RFC 0001](rfc-0001-reels.md) later
  grows explicit marked wrappers.

## Design Rules

The ring geometry layer should not allocate unless it is given an owning
storage wrapper explicitly. Borrowed storage should be the common path.

It should not call virtual functions and should not return `task` or `hope`.
It is a synchronous data-structure layer.

It should make two-span views ordinary. Contiguity is a requested property, not
the default truth of a ring.

It should distinguish raw capacity from constructed values. The existing
`junk<T>` name in [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp) is the
right idea: writable memory is not yet a span of live `T` objects.

It should preserve the existing hot-path discipline: higher layers can check
capacity and move buffered data without virtual calls or coroutine frames.

Zero capacity is not a rendezvous protocol at this layer. The ring component
may report capacity zero, but matching a producer with a consumer requires a
deck transition, task identity, cancellation rules, and possibly downstream
sink suspension. That belongs in [RFC 0008](rfc-0008-pushfeed-channels-and-removing-bell-wire.md),
not in pure geometry.

## Relationship To Reels

[RFC 0001: Reels](rfc-0001-reels.md) depends on visible source chunks and
borrowed projections. `chop_view` already operates over `buffer_chunks<const
Stock>`. Extracting ring geometry should make that source chunk vocabulary more
central, not replace it with contiguous spans.

A reel remains an interpretation layer over feed storage. This RFC only cleans
up the storage geometry beneath the feed.

## Relationship To io_uring

This RFC is not proposing to wrap the kernel's SQ/CQ rings directly in
`ring_region<T>`. Kernel rings have their own memory ordering and ownership
rules.

But the same vocabulary matters for runtime-owned rings adjacent to io_uring:

- task ready queues;
- provided buffer ID rings;
- completion feeds;
- userland buffers handed to fixed or provided buffer registration.

The raw uring support in [raw_uring.hpp](../../src/nxtrt/raw_uring.hpp) should
remain low-level and explicit.

## Open Questions

- Is the extracted component a single `ring_region<T>` or separate raw-storage
  and cursor types?
- How much object-lifetime support belongs in the pure layer?
- Should `rebase` be part of geometry or a byte/value helper layered above it?
- Which zero-capacity conveniences should this layer expose without taking on
  rendezvous semantics?
- What diagnostics should the layer provide before higher-level names are
  available?

## References

- [RFC 0001: Reels](rfc-0001-reels.md)
- [RFC 0006: Join as a Completion Feed](rfc-0006-join-as-a-completion-feed.md)
- [RFC 0008: Pushfeed Channels and Removing Bell/Wire](rfc-0008-pushfeed-channels-and-removing-bell-wire.md)
- [buffer-core.hpp](../../src/nxtrt/buffer-core.hpp)
- [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp)
- [buffers.hpp](../../src/nxtrt/buffers.hpp)
- [raw_uring.hpp](../../src/nxtrt/raw_uring.hpp)
