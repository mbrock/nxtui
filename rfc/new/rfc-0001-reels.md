# RFC 0001: Reels {#rfc_reels}

Status: new

## Summary

A reel is a causal framing facade over a byte feed.

It observes bytes from an upstream `bytefeed`, scans the currently visible byte
prefix into a semantic frame boundary, and exposes a temporary projection of
that frame without pretending that the frame is an ordinary queued value.

This fills the space between:

- `bytefeed`: an unsemantic byte stream
- `feed<T>`: a stream whose buffer stores `T` values

A reel is not a third storage ontology. The storage is still byte-shaped. The
reel changes the unit of observation from bytes to frames.

Another way to say the layering:

```cpp
feed<byte>       raw becoming
reel<Frame>      marked becoming
feed<T>          valued becoming
```

There is still one tape, but it has scenes now.

## Motivation

Some protocols are naturally byte streams with semantic frames:

- MTProto abridged frames
- TLS records
- HTTP chunks
- server-sent events
- terminal escape/input events
- JSON tokens or JSON messages

We often want feed-like operations over those semantic units:

- `peek` the next frame for dispatch
- `peek` a small window of frames for lookahead
- project the next frame
- project several frames without consuming them
- advance past the projected frame
- discard frames without materializing owned values
- preserve backpressure and visible buffering

But `feed<Frame>` is not a clean fit when `Frame` is a view into bytes. A
`feed<T>` buffers an array or ring of `T` values. A protocol frame is usually a
variable-length byte region plus metadata. Storing a queue of view objects can
hide the true backing lifetime and create fragile APIs.

Reels make the byte backing explicit.

A reel is a cadastral layer over byte territory. The bytefeed buffer is land.
The scanner recognizes parcel boundaries. The frame is a temporary right of
access to a parcel. Developing a frame into an owned value surveys, extracts,
and registers something independent.

## Terminology

Stock:

The byte storage visible through the underlying `bytefeed`. A reel may wrap a
bytefeed that already has enough stock, or it may construct/own an intermediary
bytefeed with a caller-provided buffer. Either way, the storage is still bytes.

Mark:

Optional metadata caching a recognized frame boundary or parsed header. Marks
are an optimization for lookahead and repeated dispatch. They are not the core
storage model.

Frame:

A move-only temporary projection built from the currently visible byte prefix. A
frame is not a freestanding value.

Extent:

The number of bytes occupied by the projected frame. The frame lops itself off
the byte view by reporting this extent.

Develop:

Explicitly copy or decode a frame projection into an owned value.

## Core Invariants

A reel consumes bytes causally but exposes only frame-boundary motion.

The underlying bytefeed cursor is advanced only by reel operations while the
reel is in use.

The bytefeed buffer may contain:

- a partial current frame
- one or more complete frames
- bytes after a complete frame

The reel frame cursor is always on a frame boundary.

Frame projections are borrowed. Their validity is governed by the reel and its
underlying bytefeed buffer.

This matches the existing feed rule for `buffered()`: borrowed chunk views are
invalidated by operations that refill, stream, discard, or consume the source.
Reels should preserve that discipline at the frame level.

The core operation is:

```cpp
scan feed.buffered() -> project frame -> discard frame.extent()
```

The initial validity rule should be strict:

Frames are valid until the next reel operation.

This keeps the default ontology simple. A frame is a projection, not a deed. An
explicit pin or lease type can be added later for protocols that need several
live projections at once.

## Buffer Discipline

Reels should keep byte buffering visible at construction.

For example:

```cpp
auto body_stock = std::array<std::byte, 64 * 1024>{};
auto body = response_body_decoding_reader{tls, head, body_stock};
auto events = sse_reel{body};
```

This declares:

- up to 64 KiB of byte stock at the body layer
- SSE frame projections borrow from that byte stock
- backpressure occurs when the bytefeed cannot make frame progress

If the reel needs an intermediary bytefeed, that buffer should also be visible:

```cpp
auto mt_stock = std::array<std::byte, 128 * 1024>{};
auto frames = mtproto_reel{transport, mt_stock};
```

This still does not make frames into stored values. It makes the intermediary
bytefeed stock explicit.

Optional mark buffers can declare lookahead:

```cpp
auto marks = std::array<sse_mark, 2>{};
auto events = sse_reel{body, marks};
```

This declares that the reel may remember up to two frame boundaries, while the
bytes remain in the bytefeed. The mark capacity is a visible lookahead budget,
analogous to a `feed<T>` value-buffer capacity but storing only frame metadata.

## Relationship To Feeds

`feed<T>` buffers values:

```cpp
capacity = N values
```

`reel<Frame>` observes bytefeed storage and projects frames:

```cpp
capacity = underlying bytefeed bytes + optional marks
```

Both expose stream-like backpressure. Their storage models are different:
`feed<T>` stores values, while a reel scans and discards byte prefixes.

A reel may provide feed-like methods, but the core verbs should make borrowing
visible:

```cpp
hope<std::optional<Frame>> peek();
hope<frame_window<Frame>> peek(std::size_t n);
hope<void> discard();
hope<void> discard(std::size_t n);
```

`take()` should not be the primary operation for borrowed frame projections.
Taking an `int` from a `feed<int>` makes sense: the value leaves the feed and
belongs to the caller. Taking a buffer view is more delicate: the projection
does not leave its bytefeed territory. The bytefeed has byte-level `take(n)`
conveniences for contiguous borrowed spans, but a semantic reel should make
projection and discarding separate by default.

A reel may offer `take()` only when it returns an owned or copyable value:

```cpp
hope<std::optional<OwnedFrame>> take_developed();
```

These should be `hope`-returning operations so already-buffered complete frames
can be projected synchronously, matching the hot path discipline of feeds and
sinks.

Because frames are valid only until the next reel operation, callers should
process a frame or frame window lexically before calling back into the reel:

```cpp
while (auto event = co_await events.peek()) {
    process(*event);
    (void)co_await events.discard();
}
```

Multi-frame lookahead should be equally natural:

```cpp
auto window = co_await frames.peek(2);
if (window.size() == 2 && is_pair(window[0], window[1]))
    process_pair(window[0], window[1]);
(void)co_await frames.discard(window.size());
```

Here `window.size()` is a frame count, not a byte count. The reel translates
that count into the summed byte extent of the corresponding projected frames.

If a caller needs to keep information beyond the next reel operation, it should
develop the frame into an owned value.

## Minimal Frame Interface

The smallest useful reel does not need a mark buffer. The frame type can scan
the feed's currently buffered chunks and report the byte extent it occupies.
In current nxtrt vocabulary those chunks are `byte_chunks<const std::byte>`,
the byte specialization of `value_chunks<T>`.

Example concept:

```cpp
template<typename Frame>
concept frame_view =
    requires(byte_chunks<const std::byte> bytes, Frame frame) {
        { Frame::scan(bytes) } -> std::same_as<std::optional<Frame>>;
        { frame.extent() } -> std::same_as<std::size_t>;
    };
```

Then a reel is nearly categorical for the one-frame case:

```cpp
template<frame_view Frame>
class reel {
public:
    hope<std::optional<Frame>> peek();
    hope<frame_window<Frame>> peek(std::size_t n);
    hope<void> discard();
    hope<void> discard(std::size_t n);

private:
    bytefeed & bytes_;
};
```

`peek()` scans `bytes_.buffered()`. If the buffered prefix is incomplete, it
uses `fill(n)` to ask the bytefeed for enough additional stock, then scans
again. Already-buffered complete frames should return through `hope::ready`,
with no coroutine frame and no deck round-trip.

If the scanner needs more bytes than the underlying feed can buffer, the feed
will report that through the same buffer-capacity errors used today. Declaring a
bytefeed buffer therefore also declares the maximum frame extent that can be
projected without an intermediary bytefeed.

`peek(n)` returns a projected frame window. It repeats the same scan from
successive frame boundaries until it can project up to `n` frames, EOF is
reached before the first frame, or the reel's lookahead budget is exhausted. A
minimal reel can implement `peek()` only. A reel with a mark buffer can make
`peek(n)` cheap by caching extents and parsed headers while all frame bytes
remain in the underlying bytefeed.

`discard()` after a successful `peek()` discards the currently projected frame:

```cpp
bytes_.discard(frame.extent())
```

The underlying `bytefeed::discard(limit)` returns `fare_t` because it may
discard up to `limit` values from an arbitrary stream. A reel has a stronger
post-`peek()` invariant: the complete frame extent is already buffered, so reel
`discard()` should require exact progress and present `hope<void>` to callers.

`discard(n)` after a successful `peek(n)` discards the exact byte extent of the
first `n` projected frames.

The implementation preserves the frame or frame-window projection until the next
reel operation. That next operation may discard, rebase, or refill the bytefeed
buffer.

The important law:

```cpp
Frame is a view that knows how much byte prefix it occupies.
```

This lets a frame lop itself off the byte view.

For efficient scanners, `std::optional<Frame>` may be too small. A richer scan
result can distinguish a complete frame from "need more bytes" and can report
the next useful buffered size:

```cpp
struct need_more {
    std::size_t minimum_buffered = 0;
};

template<typename Frame>
using scan_result = std::variant<Frame, need_more>;
```

That lets a length-prefixed frame ask for the complete payload immediately,
while a delimiter-scanned frame can ask for one more byte or for capacity to be
rebased. Protocol errors can still be thrown from `scan`.

Reel scanners should prefer `buffered()` / `byte_chunks` over contiguous helpers
such as `buffered_span()` or `peek_span(n)`. Contiguity should be requested only
when the protocol or downstream API actually requires it.

## Frame Requirements

A frame type should be a projection token, not an owned value.

Expected properties:

- move-only, or at least semantically non-copyable
- cheap to construct from visible byte chunks
- reports its byte extent
- exposes segmented views where possible
- does not require contiguizing frame bytes
- clearly documents when it becomes invalid
- can be explicitly developed into an owned value when needed

Example shape:

```cpp
struct mtproto_frame {
    byte_chunks<const std::byte> payload() const;
    std::uint32_t constructor() const;
};

struct sse_event_frame {
    byte_chunks<const std::byte> type() const;
    byte_chunks<const std::byte> id() const;
    segmented_bytes data() const; // name TBD
    std::optional<int> retry_ms() const;
};
```

Accessors should prefer chunk views or ranges of spans over contiguous strings
or byte spans unless contiguity is intrinsic to the protocol frame.

## SSE Example

The current SSE feed parses borrowed lines from a bytefeed and copies event
fields into owned `std::string` members. This is safe because
`server_sent_event` is an owned value, so `feed<server_sent_event>` is valid.

A reel-shaped SSE parser could instead scan the body bytefeed for event
boundaries and return borrowed event projections:

```cpp
auto body_stock = std::array<std::byte, 64 * 1024>{};
auto body = response_body_decoding_reader{tls, head, body_stock};
auto events = sse_reel<sse_event_frame>{body};

while (auto event = co_await events.peek()) {
    for (auto chunk : event->data())
        json.feed(chunk);
    (void)co_await events.discard();
}
```

Multi-line `data:` fields do not necessarily need to be contiguized. The event
projection can expose a segmented data view, including virtual newline
separators if the downstream parser accepts chunk ranges.

## MTProto Example

MTProto abridged framing is naturally reel-shaped:

```cpp
auto transport_stock = std::array<std::byte, 128 * 1024>{};
auto transport = socket_source{fd, transport_stock};
auto frames = mtproto_reel<mtproto_frame>{transport};

auto frame = co_await frames.peek();
auto plain = frame->plain_message();
auth.receive_res_pq(plain.body(), ...);
(void)co_await frames.discard();
```

The frame projection can expose the raw payload chunks, parsed quick-ack state,
plain-message fields, encrypted-message fields, and TL constructor dispatch
without copying the payload into an owned message.

## Open Questions

- Which protocols need explicit pin or lease frames beyond the default
  valid-until-next-operation rule?
- Which protocols need `peek(n)` and an optional mark cache, rather than only
  `peek()` for the next frame?
- What is the minimum common interface for segmented byte views?
- Should the minimal scanner use `std::optional<Frame>`, or a richer
  `scan_result` that reports the next useful `fill(n)` target?
- Can every reel be expressed as a facade over `bytefeed`, or do some protocols
  want a named intermediary bytefeed/reel wrapper for clarity?
- How should scanner errors preserve enough context for debugging without
  copying frame bytes?

## Non-Goals

Reels are not a replacement for `feed<T>` when `T` is an actual value.

Reels are not required to avoid all copying. They make copying explicit and
localized. A reel implementation may wrap an intermediary bytefeed with its own
visible buffer so lower layers can advance safely.

Reels do not require every protocol parser to become generic immediately. They
are a vocabulary and design target for protocols where framed byte stock is the
real storage.
