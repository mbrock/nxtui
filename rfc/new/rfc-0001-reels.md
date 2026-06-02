# RFC 0001: Reels {#rfc_reels}

Status: new

## Summary

A reel is a causal framing facade over a source feed.

It observes stock from an upstream `feed<Stock>`, scans the currently visible
source prefix into a semantic frame boundary, and exposes a temporary
projection of that frame without pretending that the frame is an ordinary
queued value.

The pure projection operation is a `chop`: a stateless range view over
currently visible stock. A chop yields `(extent, frame)` pairs. The extent is
the number of source values the frame occupies, so the next chop starts exactly
where the previous one ends.

This fills the space between:

- `feed<Stock>`: an unsemantic source stream
- `feed<T>`: a stream whose buffer stores developed `T` values

`bytefeed` is just the important specialization:

```cpp
using bytefeed = feed<std::byte>;
```

A reel is not a third storage ontology. The storage still belongs to the
underlying feed. The reel changes the unit of observation from source values to
frames.

Another way to say the layering:

```cpp
feed<Stock>          raw stock becoming
chop<Stock, Frame>   visible lops
reel<Stock, Frame>   causal framing
feed<T>              valued becoming
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

But `feed<Frame>` is not a clean fit when `Frame` is a view into earlier stock.
A `feed<T>` buffers an array or ring of `T` values. A protocol frame is usually
a variable-length region of source stock plus metadata. Storing a queue of view
objects can hide the true backing lifetime and create fragile APIs.

Reels make the backing feed explicit.

A chop is a cadastral layer over source territory. The feed buffer is land. The
scanner recognizes parcel boundaries. The frame is a temporary right of access
to a parcel. Developing a frame into an owned value surveys, extracts, and
registers something independent. The reel is only the steward that asks for
more land to be visible and advances the feed cursor at parcel boundaries.

## Terminology

Stock:

The source values visible through the underlying `feed<Stock>`. For byte
protocols the stock type is `std::byte`; for other layers it may be
`std::uint32_t`, tokens, decoded codepoints, or any value type that the feed
stores honestly. A reel may wrap a feed that already has enough stock, or it
may construct/own an intermediary feed with a caller-provided buffer. Either
way, the storage model remains the storage model of that feed.

Mark:

Optional metadata caching a recognized frame boundary or parsed header. Marks
are an explicit optimization outside the base reel. They are not the core
storage model, and a plain reel should not allocate or own them.

Chop:

A stateless range view over currently visible source chunks. It starts at
source offset zero, repeatedly scans the current suffix, yields
`(extent, frame)`, and advances the view cursor by `extent`. Re-iterating a
chop re-scans hot buffered memory instead of consulting stored marks.

Frame:

A move-only temporary projection built from the currently visible source
prefix. A frame is not a freestanding value.

Extent:

The number of source values occupied by the projected frame. For
`feed<std::byte>` this is a byte count. The frame lops itself off the source
view by reporting this extent.

Develop:

Explicitly copy or decode a frame projection into an owned value.

## Core Invariants

A reel consumes source stock causally but exposes only frame-boundary motion.

The underlying feed cursor is advanced only by reel operations while the reel
is in use.

The feed buffer may contain:

- a partial current frame
- one or more complete frames
- stock after a complete frame

The reel frame cursor is always on a frame boundary.

Frame projections are borrowed. Their validity is governed by the chop/reel and
the underlying feed buffer.

This matches the existing feed rule for `buffered()`: borrowed chunk views are
invalidated by operations that refill, stream, discard, or consume the source.
Reels should preserve that discipline at the frame level.

The core operation is:

```cpp
chop(feed.buffered()) -> observe (extent, frame) -> discard extent
```

The initial validity rule should be strict:

Chops and frames are valid until the next reel or feed operation that may mutate
the underlying source buffer.

This keeps the default ontology simple. A frame is a projection, not a deed. An
explicit pin or lease type can be added later for protocols that need several
live projections at once.

## Buffer Discipline

Reels should not hide buffering. The base reel does not own source stock, mark
buffers, or frame caches.

For example:

```cpp
auto body_stock = std::array<std::byte, 64 * 1024>{};
auto body = response_body_decoding_reader{tls, head, body_stock};
auto events = reel<std::byte, sse_event_frame>{body};
```

This declares:

- up to 64 KiB of byte stock at the body layer
- SSE frame projections borrow from that byte stock
- backpressure occurs when the byte feed cannot make frame progress

If a protocol layer needs an intermediary feed, that buffer should also be
visible:

```cpp
auto mt_stock = std::array<std::byte, 128 * 1024>{};
auto transport = mtproto_transport{socket, mt_stock};
auto frames = reel<std::byte, mtproto_frame>{transport};
```

This still does not make frames into stored values. It makes the intermediary
byte feed stock explicit.

If a future optimized layer wants cached marks, the mark buffer should be a
separate visible asset:

```cpp
auto marks = std::array<sse_mark, 2>{};
auto events = marked_sse_reel{body, marks};
```

This declares that an optimized wrapper may remember up to two frame boundaries,
while the bytes remain in the byte feed. The mark capacity is a visible
lookahead budget, analogous to a `feed<T>` value-buffer capacity but storing
only frame metadata.

## Relationship To Feeds

`feed<T>` buffers values:

```cpp
capacity = N values
```

`chop<Stock, Frame>` observes feed storage and projects frames:

```cpp
capacity = underlying feed stock
```

`reel<Stock, Frame>` adds causal feed operations around that projection:

```cpp
capacity = underlying feed stock
```

Both expose stream-like backpressure. Their storage models are different:
`feed<T>` stores values, while a chop scans source prefixes and a reel fills or
discards the underlying feed at those extents.

A reel may provide feed-like methods, but the core verbs should make borrowing
visible:

```cpp
chop_view<Stock, Frame> visible() const;
hope<chop_view<Stock, Frame>> peek(std::size_t minimum_count = 1);
hope<void> discard_prefix(std::size_t extent);
```

`take()` should not be the primary operation for borrowed frame projections.
Taking an `int` from a `feed<int>` makes sense: the value leaves the feed and
belongs to the caller. Taking a buffer view is more delicate: the projection
does not leave its source territory. The bytefeed has byte-level `take(n)`
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
process a chop lexically before calling back into the reel:

```cpp
while (true) {
    auto events_now = co_await events.peek();
    if (events_now.empty())
        break;

    auto event = *events_now.begin();
    process(event.frame);
    (void)co_await events.discard_prefix(event.extent);
}
```

Multi-frame lookahead should be equally natural:

```cpp
auto window = co_await frames.peek(2);
if (window.has_at_least(2)) {
    auto first_two = window | std::views::take(2);
    if (!is_pair(first_two))
        co_return;
    process_pair(first_two);
    (void)co_await frames.discard_prefix(chop_extent(first_two));
}
```

Here lookahead is a lazy iterable view, not a stored array of frame values. The
caller computes the summed extent of the observed chops and asks the reel to
discard that exact source prefix.

If a caller needs to keep information beyond the next reel operation, it should
develop the frame into an owned value.

## Chop: Stateless Lopping View {#rfc_reels_chop}

The smallest useful framing unit is not the reel. It is a `chop`: a stateless
view over currently visible source chunks. The frame type can scan the current
suffix and report the extent it occupies. In current nxtrt vocabulary those
chunks are `value_chunks<const Stock>`; `byte_chunks<const std::byte>` is the
byte specialization used by byte protocols.

Example concept:

```cpp
template<typename Stock, typename Frame>
concept chop_scanner =
    requires(value_chunks<const Stock> stock) {
        { Frame::scan(stock) } -> std::same_as<chop_scan_result<Frame>>;
    };
```

A complete scan returns a `(extent, frame)` pair:

```cpp
template<typename Frame>
struct frame_chop {
    std::size_t extent;
    Frame frame;
};

struct chop_need_more {
    std::size_t minimum_buffered = 0;
};

template<typename Frame>
using chop_scan_result = std::variant<frame_chop<Frame>, chop_need_more>;
```

The view itself is range-shaped:

```cpp
auto window = chop<std::byte, mtproto_frame>(bytes.buffered());

for (auto const & [extent, frame] : window | std::views::take(2))
    dispatch(frame);
```

The view stores only the source chunks and the scanner. It does not cache frame
boundaries. Each traversal recomputes them by scanning hot buffered memory.
That is the point: recomputation is cheaper and clearer than inventing an
implicit frame buffer.

## Reel: Causal Feed Facade {#rfc_reels_reel}

Then a reel is nearly categorical:

```cpp
template<typename Stock, typename Frame>
class reel {
public:
    chop_view<Stock, Frame> visible() const;
    hope<chop_view<Stock, Frame>> peek(std::size_t minimum_count = 1);
    hope<void> discard_prefix(std::size_t extent);

private:
    feed<Stock> & source_;
};
```

`visible()` is pure projection over `source_.buffered()`.

`peek(n)` scans `source_.buffered()`. If fewer than `n` complete frames are
visible, it uses `fill(n)`-shaped feed operations to ask for enough additional
stock, then scans again. Already-buffered complete frames should return through
`hope::ready`, with no coroutine frame and no deck round-trip.

If the scanner needs more source values than the underlying feed can buffer,
the feed will report that through the same buffer-capacity errors used today.
Declaring the feed buffer therefore also declares the maximum frame extent that
can be projected without an intermediary feed.

`peek(n)` returns another `chop_view`. It repeats the same scan from successive
frame boundaries until at least `n` frames are visible or the feed reports
EOF/capacity failure. No frame array is produced.

`discard_prefix(extent)` after a successful `peek()` discards the source prefix
occupied by the observed chop or chop window:

```cpp
source_.discard(extent)
```

The underlying `feed<Stock>::discard(limit)` returns `fare_t` because it may
discard up to `limit` values from an arbitrary stream. A reel has a stronger
post-`peek()` invariant: the complete extent is already buffered, so reel
`discard_prefix()` should require exact progress and present `hope<void>` to
callers.

The implementation preserves the chop projection until the next reel operation.
That next operation may discard, rebase, or refill the source feed buffer.

The important law:

```cpp
Frame is a view that knows how much source prefix it occupies.
```

This lets a frame lop itself off the source view.

`chop_need_more` lets a length-prefixed frame ask for the complete payload
immediately, while a delimiter-scanned frame can ask for one more source value
or for capacity to be rebased. Protocol errors can still be thrown from `scan`.

Reel scanners should prefer `buffered()` / chunk views over contiguous helpers
such as `buffered_span()` or `peek_span(n)`. Contiguity should be requested
only when the protocol or downstream API actually requires it.

## Frame Requirements

A frame type should be a projection token, not an owned value.

Expected properties:

- move-only, or at least semantically non-copyable
- cheap to construct from visible source chunks
- reports its source extent
- exposes segmented views where possible
- does not require contiguizing frame stock
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
auto events = reel<std::byte, sse_event_frame>{body};

while (true) {
    auto visible = co_await events.peek();
    if (visible.empty())
        break;

    auto event = *visible.begin();
    for (auto chunk : event.frame.data())
        json.feed(chunk);
    (void)co_await events.discard_prefix(event.extent);
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
auto frames = reel<std::byte, mtproto_frame>{transport};

auto visible = co_await frames.peek();
auto frame = *visible.begin();
auto plain = frame.frame.plain_message();
auth.receive_res_pq(plain.body(), ...);
(void)co_await frames.discard_prefix(frame.extent);
```

The frame projection can expose the raw payload chunks, parsed quick-ack state,
plain-message fields, encrypted-message fields, and TL constructor dispatch
without copying the payload into an owned message.

## Open Questions

- Which protocols need explicit pin or lease frames beyond the default
  valid-until-next-operation rule?
- Which protocols need an explicit marked wrapper, rather than recomputing
  chops from hot buffered memory?
- What is the minimum common interface for segmented byte views?
- Which non-byte source feeds naturally want reels, and what does that teach us
  about the common scanner vocabulary?
- Can every byte-protocol reel be expressed as a facade over `bytefeed`, or do
  some protocols want a named intermediary bytefeed/reel wrapper for clarity?
- How should scanner errors preserve enough context for debugging without
  copying frame bytes?

## Non-Goals

Reels are not a replacement for `feed<T>` when `T` is an actual value.

Reels are not required to avoid all copying. They make copying explicit and
localized. A reel implementation may wrap an intermediary feed with its own
visible buffer so lower layers can advance safely.

Base reels do not cache frame boundaries. A marked/cached wrapper can exist, but
its mark buffer should be explicit at construction.

Reels do not require every protocol parser to become generic immediately. They
are a vocabulary and design target for protocols where framed source stock is
the real storage.
