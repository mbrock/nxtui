# RFC 0012: Sink/Feed Fast Paths for Splice and Sendfile {#rfc_splice_sendfile}

Status: new

## Summary

Add optimized transfer hooks so suitable feeds and sinks can move data without
projecting bytes into userland.

The public streaming path should remain feed/sink shaped. The fast path is an
optional cold-path optimization: a sink can accept stock directly from a source
such as a file, socket, pipe, or kernel splice path. If no direct path exists,
the runtime falls back to the ordinary buffered copy.

## Motivation

[buffers.hpp](../../src/nxtrt/buffers.hpp) already records the design lineage:
the feed/sink family ports the important shape of Zig's post-0.15
`std.Io.Reader` and `std.Io.Writer`.

The key idea is that `stream_more()` is the primitive. Pull-shaped refill is
derived by streaming into the reader's own unused capacity. That is what makes
future direct transfer possible:

```text
file feed -> socket sink
pipe feed -> file sink
TLS plaintext feed -> hash sink
```

Today the API has the right broad shape, but concrete fd/socket feeds and
sinks still mostly move ordinary spans.

## Proposal

Give sinks first refusal on direct transfer:

```cpp
hope<fare_t> sink<byte>::splice(feed<byte> & source, std::size_t limit);
```

or a more specific cold hook:

```cpp
virtual hope<fare_t> drain_from(feed<byte> & source, std::size_t limit);
```

The exact name is open. The semantic order is:

1. drain any bytes already buffered in the source;
2. ask the sink whether it can transfer directly from this source;
3. if not, fall back to `source.stream(*this, limit)`.

The sink gets first refusal because the sink knows what it can accept without
projection: file, socket, pipe, TLS, memory sink, hash sink, compression sink,
and so on.

## Examples

File to socket:

```text
filefeed -> socket_sink
sendfile/copy_file_range if possible
buffered copy otherwise
```

Socket to file:

```text
socket_source -> fd_sink
splice/readv/writev if possible
buffered copy otherwise
```

HTTP body skip:

```text
body feed -> discarding sink
discard_more can skip without copying bytes through userland
```

TLS is a boundary case. Encrypted socket bytes cannot be sent directly as
plaintext without passing through TLS state, but TLS may still offer direct
transfer from its plaintext buffer to compatible sinks.

## Current Hooks

Current related hooks:

- `feed<T>::stream(sink<T>&, limit)`;
- `feed<T>::stream_more(sink<T>&, limit)`;
- `feed<T>::discard(limit)` and `discard_more(limit)`;
- `sink<T>::drain_more(values, splat)`;
- byte `read_vec()` and `write()` convenience paths.

The TODOs in [buffers.hpp](../../src/nxtrt/buffers.hpp) already name the work:

- teach fd/socket feeds and sinks about each other;
- teach sources real scatter reads;
- add optimized `discard_more(limit)`;
- support rebase implementations beyond memmove.

This RFC turns those TODOs into the runtime-level design target.

## Invariants

Direct transfer must respect bytes already buffered in the source. Buffered
stock is causally earlier than bytes still in the kernel or file.

A direct transfer reports the number of bytes logically accepted by the sink,
not necessarily the number of bytes copied through any particular address.

If the direct path cannot make progress, it must fall back or report a real
would-block/error condition. It must not spin.

Borrowed views exposed by feeds remain invalidated by the same operations as
today. Direct transfer does not grant callers hidden stable pointers.

## Relationship To Other RFCs

[RFC 0007](../cur/rfc-0007-ring-geometry-extraction.md) keeps the buffer geometry
clean enough for direct paths to reason about what is already buffered.

[RFC 0009](rfc-0009-wishes-urges-and-provided-buffers.md) and
[RFC 0010](rfc-0010-firm-buffer-groups-and-io-land.md) provide the territory
vocabulary for direct receive/send paths.

[RFC 0001](rfc-0001-reels.md) remains an interpretation layer above source
stock. Reels should not force direct byte transfer to materialize semantic
frames unless a protocol actually needs owned values.

## Open Questions

- Is the direct-transfer hook on `sink`, on `feed`, or a double-dispatch helper?
- Which platform operations are worth supporting first: `sendfile`,
  `copy_file_range`, `splice`, `readv/writev`, or io_uring variants?
- How do direct paths interact with TLS, compression, hashing, and tracing
  sinks?
- Can `discard_more(limit)` share the same direct-transfer machinery through a
  special sink?
- What diagnostics should distinguish fallback copy from direct transfer?

## References

- [RFC 0007: Ring Geometry Extraction](../cur/rfc-0007-ring-geometry-extraction.md)
- [RFC 0009: Wishes, Urges, and Provided Buffers](rfc-0009-wishes-urges-and-provided-buffers.md)
- [RFC 0001: Reels](rfc-0001-reels.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [buffers.hpp](../../src/nxtrt/buffers.hpp)
- [value-buffers.hpp](../../src/nxtrt/value-buffers.hpp)
- [wish_ops.hpp](../../src/nxtrt/wish_ops.hpp)
