# Runtime RFCs {#runtime_rfcs}

The RFCs are design notes for the runtime vocabulary, current experiments, and
future implementation paths. Some describe code that already exists; some are
speculative scaffolding.

## Current synthesis

[Recipes, pools, and structured async](rt-concurrency-direction.md) connects
the implemented idea/pool work with the next composition and Wisp effect-bridge
decisions. It distinguishes current behavior from proposed teams, coping and
terminal consumers, and records the migration targets to reconcile with the
parallel Wisp/HTTP work. For the concrete pool API, see
[Bounded idea pools](rt-pool.md).

## Current RFCs

- [RFC 0003: Deck Task Registry and Task IDs](../rfc/cur/rfc-0003-deck-task-registry.md)
- [RFC 0007: Ring Geometry Extraction](../rfc/cur/rfc-0007-ring-geometry-extraction.md)
- [RFC 0013: Runtime Env Core Fields](../rfc/cur/rfc-0013-runtime-env-core-fields.md)

## Superseded RFCs

- [RFC 0002: Firm Frame Arenas](../rfc/cur/rfc-0002-firm-frame-arenas.md)
  — removed; coroutine frames use the ordinary allocator.
- [RFC 0005: Firm Bookkeeping without Heap Vectors](../rfc/cur/rfc-0005-firm-bookkeeping-without-heap-vectors.md)
  — firms and their child bookkeeping have since been removed (RFC 0019).

## New RFCs

- [RFC 0000: Prolegomena to NXT System Theory](../rfc/new/rfc-0000-prolegomena.md)
- [RFC 0001: Reels](../rfc/new/rfc-0001-reels.md)
- [RFC 0004: Wand Completion Routing without exec Hub](../rfc/new/rfc-0004-wand-completion-routing.md)
- [RFC 0006: Join as a Completion Feed](../rfc/new/rfc-0006-join-as-a-completion-feed.md)
- [RFC 0008: Pushfeed Channels and Removing Bell/Wire](../rfc/new/rfc-0008-pushfeed-channels-and-removing-bell-wire.md)
- [RFC 0009: Wishes, Urges, and Provided Buffers](../rfc/new/rfc-0009-wishes-urges-and-provided-buffers.md)
- [RFC 0010: Firm Buffer Groups and I/O Land](../rfc/new/rfc-0010-firm-buffer-groups-and-io-land.md)
- [RFC 0011: Multishot Wishes as Feeds](../rfc/new/rfc-0011-multishot-wishes-as-feeds.md)
- [RFC 0012: Sink/Feed Fast Paths for Splice and Sendfile](../rfc/new/rfc-0012-splice-and-sendfile-fast-paths.md)
- [RFC 0014: Idea Algebra](../rfc/new/rfc-0014-idea-algebra.md)
- [RFC 0015: Async RAII Resources](../rfc/new/rfc-0015-async-raii-resources.md)
- [RFC 0016: Temporal Algebras](../rfc/new/rfc-0016-temporal-algebras.md)
- [State of the Frontier](../rfc/new/rfc-0017-state-of-the-frontier.md)
- [RFC 0018: Portable Wisp Lisp Machines](../rfc/new/rfc-0018-portable-wisp-lisp-machines.md)
- [RFC 0019: Firms Without Bodies](../rfc/new/rfc-0019-firms-without-bodies.md)
  — implemented: fixed groups (`settle` with `group` subclasses) and streaming
  pools (`drain`) replace firms.
- @ref rfc_wisp_semantic_code "RFC 0020: Wisp Semantic Code and Bytecode"
  — record stages implemented: a Wisp compiler over semantic records, a graph
  checker, and heap-resident prepared execution, with measurements. Lowering
  continues in RFC 0021.
- @ref rfc_wisp_lowered_code "RFC 0021: Wisp Lowered Code"
  — proposed: lower checked records into compact executable nodes that
  neither decode nor retain the IR, keeping multi-shot frames and tapes.
