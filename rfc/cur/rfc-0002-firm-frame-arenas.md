# RFC 0002: Firm Frame Arenas {#rfc_firm_frame_arenas}

Status: current

## Summary

A firm should become the required allocation scope for coroutine frames.

Today a `task<T>` coroutine frame is owned by the `task<T>` object and
allocated through the compiler's ordinary coroutine allocation path. A firm
owns the structured lifetime of forked children, but not yet the memory land
their frames occupy.

This RFC proposes that every task belongs to a firm and every task frame is
allocated from firm-owned or firm-borrowed frame land. Ordinary recursion
consumes vertical stack space; concurrent breadth consumes firm frame space. In
the language of [RFC 0000](../new/rfc-0000-prolegomena.md), the firm becomes the
visible territory for held async work, not only the object that later joins it.

## Series Note

The runtime RFCs after [RFC 0001: Reels](../new/rfc-0001-reels.md) are intended as a
staged consolidation of where held work lives:

- [RFC 0007: Ring Geometry Extraction](rfc-0007-ring-geometry-extraction.md)
  factors out the reusable buffer geometry.
- [RFC 0003: Deck Task Registry and Task IDs](rfc-0003-deck-task-registry.md)
  makes task identity durable enough to route completions.
- This RFC gives coroutine frames explicit land.
- [RFC 0013: Runtime Env Core Fields](rfc-0013-runtime-env-core-fields.md)
  makes the current deck, firm, wand, and task hot fields.
- [RFC 0004: Wand Completion Routing without exec Hub](../new/rfc-0004-wand-completion-routing.md)
  uses the new task identity to route one-shot completions.
- [RFC 0009](../new/rfc-0009-wishes-urges-and-provided-buffers.md) and
  [RFC 0010](../new/rfc-0010-firm-buffer-groups-and-io-land.md) move I/O byte land
  into firm and wand territory.
- [RFC 0008: Pushfeed Channels and Removing Bell/Wire](../new/rfc-0008-pushfeed-channels-and-removing-bell-wire.md)
  removes the backend trip from internal synchronization.
- [RFC 0014: Idea Algebra](../new/rfc-0014-idea-algebra.md) moves user-facing
  composition toward task factories.
- [RFC 0015: Async RAII Resources](../new/rfc-0015-async-raii-resources.md) treats
  long-lived firm children as scoped resources.

The most important immediate pair is still this RFC and
[RFC 0003](rfc-0003-deck-task-registry.md): the firm owns frame land, while the
deck owns task identity.

## Initial Implementation Plan

The first implementation plan should stay narrow:

1. Extract the pure ring geometry from [RFC 0007](rfc-0007-ring-geometry-extraction.md).
2. Add root-firm entrypoints and make task construction require a current firm.
3. Build a simple ring-shaped frame allocator over borrowed firm bytes.
4. Add a borrowed deck task table with compact 32-bit task IDs, likely
   `u24 index + u8 era`.
5. Replace heap/shared-pointer firm child records with bounded firm
   bookkeeping and deed result evacuation.
6. Make firm join consume a completion feed.
7. Replace `wire`/`bell` internal coordination with a `pushfeed<T>` subclass
   using single producer/consumer wait slots.

Only after that should the wand-routing and provided-buffer RFCs become the
main implementation focus. They need the firm/deck/channel substrate to stop
being speculative.

## Motivation

The runtime notes already describe a firm as a structured holder of child work.
See [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
and [Behavioral threads as occurrent structure](../../docs/rt-occurrents.md).
The occurrent note is especially direct: a firm is a place, and the child task
histories happen inside that place.

The current C++ code has the lifetime half of that idea. `firm::fork()` stores
child records and joins or stops children before the firm is allowed to finish.
But the child coroutine frame itself still comes from the ordinary coroutine
allocator, and the firm bookkeeping uses heap objects. That leaves the most
important land invisible.

Making the frame arena explicit gives us:

- bounded memory for concurrent breadth;
- diagnostics for frame pressure;
- a direct correspondence between structured concurrency and memory ownership;
- a path to avoiding accidental heap growth in hot async paths;
- a firmer basis for task IDs and direct wand completion routing.

## Current Shape

The current frame and firm pieces live mostly in
[task.hpp](../../src/nxtrt/task.hpp):

- `detail::promise_base` owns per-task control state, result storage,
  continuation, stop callbacks, and the task's ambient `runtime_env`.
- `task<T>` uniquely owns the coroutine handle until it is awaited, started,
  or forked.
- `firm::fork(task<T>)` releases a task handle, stores a shared child record,
  installs a completion callback, and enqueues the child on the active deck.
- `deed<T>` and `catching_deed<T>` are shared-pointer handles to child records.

The current conceptual model in [runtime.rkt](../../nxtrt/runtime.rkt) already
distinguishes firms, tasks, deeds, wishes, and execs. The model files are still
an experiment, so this RFC should not require model work before implementation;
it is enough to keep the vocabulary in mind when the model is next revised.

## Proposal

A firm has a frame arena:

```cpp
class firm {
public:
    explicit firm(frame_arena_ref frames, firm_storage_ref bookkeeping);
};
```

The first implementation should be deliberately plain and borrowed: a bounded
frame region over caller-provided bytes, with alignment support and explicit
overflow diagnostics. Because [RFC 0007](rfc-0007-ring-geometry-extraction.md)
already extracts ring geometry, the likely first allocator is ring-shaped: task
frames are allocated at the tail, and contiguous retired prefixes can be tossed
when every frame in that prefix is free.

That is not a general heap. It is closer to a feed of frame land: allocate from
the back, retire from the front when structured settlement makes the prefix
dead, and report pressure when the ring cannot fit the next frame. Later
implementations can add slabs, per-size classes, or debug poisoning if the ring
shape is too restrictive.

Coroutine promise allocation should consult the current firm through the hot
runtime environment. A task that is born without a current firm is a runtime
error. Root execution creates or receives a root firm before the root task is
constructed; there is no compatibility mode where ordinary task creation falls
back to the heap.

Frame allocation is bounded and fallible. Allocation failure should report at
least:

- requested frame size;
- requested alignment;
- remaining arena capacity;
- firm id or debug name;
- frame arena high-water mark;
- current task id when available.

This should be a structured runtime diagnostic, not an unexplained
`std::bad_alloc`. It should use the existing exception and stacktrace path in
[exceptions.hpp](../../src/nxtrt/exceptions.hpp) and
[stacktrace.hpp](../../src/nxt/stacktrace.hpp) where that helps explain where
the frame allocation was attempted.

## Invariants

A task frame allocated from firm land may not outlive that firm.

A firm may not release or reuse frame land until every child history located in
that land has settled, been joined, or been cancelled. This is the memory-side
version of structured concurrency.

A frame pointer stored in the deck task registry names memory owned by a firm,
not memory owned by the deck. The deck can identify and schedule the task, but
it does not become the allocator for the frame.

Tasks created outside a firm are invalid. If a caller wants root work, it must
enter a root firm first. This makes the rule simple enough to trust: a task
belongs to a firm.

## API Direction

Prefer task factories over preconstructed tasks when forking:

```cpp
auto child = fork(fn, args...);
```

instead of only:

```cpp
auto child = fork(fn(args...));
```

That lets task construction happen inside the target firm. It also avoids the
dangerous pattern of creating a coroutine lambda that captures state and then
letting the returned task outlive the lambda closure.

The current `fork(task<T>)` overload can remain only as a temporary migration
primitive while the implementation changes. The target API is firm-local
construction, because a preconstructed task has already missed the allocator
decision.

## Relationship To Other RFCs

[RFC 0003](rfc-0003-deck-task-registry.md) moves hot task metadata out of
promises and into a deck registry. This RFC does the complementary move for
memory: the promise/frame remains in firm land, while the deck records compact
identity and scheduling state.

[RFC 0013](rfc-0013-runtime-env-core-fields.md) provides the hot `current_firm`
field needed by promise allocation.

[RFC 0010](../new/rfc-0010-firm-buffer-groups-and-io-land.md) generalizes the same
territory idea from coroutine frames to I/O buffers.

## Open Questions

- What is the smallest ring-shaped frame allocator that can support aligned
  coroutine frames and prefix retirement?
- Do completed child frames get reused before firm settlement, or only after
  the firm joins or a contiguous frame prefix retires?
- What does the root-firm entrypoint look like when task construction itself
  requires a firm?
- What debugging hooks should expose frame high-water marks and allocation
  failures?
- Which parts of coroutine promise allocation work cleanly on the modern
  compiler floor we care about: roughly GCC 14+ and Clang 20+, without relying
  on unimplemented C++26 features?

## References

- [RFC 0000: Prolegomena to NXT System Theory](../new/rfc-0000-prolegomena.md)
- [RFC 0003: Deck Task Registry and Task IDs](rfc-0003-deck-task-registry.md)
- [Runtime Overview](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [Behavioral threads as occurrent structure](../../docs/rt-occurrents.md)
- [task.hpp](../../src/nxtrt/task.hpp)
- [env.hpp](../../src/nxtrt/env.hpp)
- [exceptions.hpp](../../src/nxtrt/exceptions.hpp)
- [stacktrace.hpp](../../src/nxt/stacktrace.hpp)
- [runtime.rkt](../../nxtrt/runtime.rkt)
