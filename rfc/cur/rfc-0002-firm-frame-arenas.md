# RFC 0002: Firm Frame Arenas {#rfc_firm_frame_arenas}

Status: implemented frame allocation; original ring/bookkeeping plans retained
as history

## Current implementation note

The frame pool remains in use: default firms grow lazy, nonmoving owned chunks;
explicitly borrowed frame land is bounded. Frames are individually recycled
using top retraction and size-class free lists, as described below.

Firm bookkeeping is now ordinary growable nursery bookkeeping, not the bounded
storage bundles proposed in this RFC's original plan. Separate deed and
completion ledgers and exact-N tuple firms have been removed; join traverses
child records. See the [RFC 0005 supersession
note](rfc-0005-firm-bookkeeping-without-heap-vectors.md).
Pool jobs now own their task handles directly while using ambient firm frame
land. Allocation scope is not structured child ownership. See
[Recipes, pools, and structured async](../../docs/rt-concurrency-direction.md)
for the direction toward separating those responsibilities.

## Summary

A firm is currently the required allocation scope for coroutine frames.
Coroutine promise allocation consults the ambient firm; frame destruction
returns the block to that firm's arena. The task handle may be owned by an
awaiting task, a firm child record, or a pool slot.

Every task frame is allocated from firm-owned or firm-borrowed frame land,
but not every task is a forked child of that firm. Nested coroutine calls as
well as concurrent breadth consume frame space. In
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

## Original implementation plan (historical)

The original sequence was:

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

The ring allocator was rejected after measurement. Bounded nursery bookkeeping
was implemented and then removed; result evacuation remains. Firm join still
traverses records, while the separate pool now supplies a bounded result feed.
Generic pushfeed channels and direct wand completion routing remain proposals,
not prerequisites that have all been completed.

## Motivation

The runtime notes already describe a firm as a structured holder of child work.
See [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
and [Behavioral threads as occurrent structure](../../docs/rt-occurrents.md).
The occurrent note is especially direct: a firm is a place, and the child task
histories happen inside that place.

Before this change, firms held child records but task frames used the ordinary
coroutine allocation path. The lifetime structure did not make frame territory
visible. Explicit frame provision addresses that gap independently of the
representation chosen for child bookkeeping.

Making the frame arena explicit gives us:

- explicit, optionally bounded frame memory;
- diagnostics for frame pressure;
- a direct correspondence between structured concurrency and memory ownership;
- a path to avoiding accidental heap growth in hot async paths;
- a firmer basis for task IDs and direct wand completion routing.

## Current Shape

The public umbrella is [task.hpp](../../src/nxtrt/task.hpp); implementation
pieces are in [task/](../../src/nxtrt/task):

- `detail::promise_base` owns per-task control state, continuation, stop
  callbacks, and the task's ambient `runtime_env`; typed promises hold results.
- `task<T>` uniquely owns its handle until ownership is explicitly transferred.
  Starting or awaiting it does not itself transfer ownership.
- `firm::fork(task<T>)` transfers the handle to an individually owned stable
  child record, installs a completion observer, and enqueues the child.
- `deed<T>` and `catching_deed<T>` contain movable result state, linked
  non-owningly to the child record; they are not shared owners of the frame.
- `pool<Idea>` instead retains task handles in borrowed slots and observes their
  completion directly, without creating firm child records.

The executable model in [runtime.rkt](../../nxtrt/runtime.rkt) distinguishes
firms, pool slots, tasks, deeds, wishes, and execs. Runtime lifecycle changes
must keep that model aligned with the intended semantics; frame-byte details
remain an implementation concern.

## Allocation API and the original ring proposal

A firm can borrow explicit frame land:

```cpp
class firm {
public:
    explicit firm(frame_storage_ref frames);
};
```

The default constructor instead provides lazy owned chunks. Both modes keep
live frame addresses stable and support individual frame reclamation.

The original borrowed implementation sketch proposed ring-shaped allocation:
allocate at the tail and retire dead prefixes. That geometry was attractive
because buffers already needed it, but it does not match ordinary coroutine
lifetimes. The measurements below explain the replacement.

Coroutine promise allocation consults the current firm through the hot
runtime environment. A task that is born without a current firm is a runtime
error. Root execution creates or receives a root firm before the root task is
constructed; there is no compatibility mode where ordinary task creation falls
back to the heap.

Borrowed frame allocation is bounded and fallible. Its exhaustion diagnostic
includes:

- requested frame size;
- requested alignment;
- remaining arena capacity;
- firm id or debug name;
- frame arena high-water mark;
- current task id when available.

Owned chunk growth can throw `std::bad_alloc`. Borrowed exhaustion uses the
existing runtime diagnostic and exception path in
[exceptions.hpp](../../src/nxtrt/exceptions.hpp) and
[stacktrace.hpp](../../src/nxt/stacktrace.hpp) where that helps explain where
the frame allocation was attempted.

## Implementation: Stack Top Plus Size-Class Reuse

The ring sketched above was measured against the echo bench before it was
built, and it does not reclaim anything there. The bench forks one long-lived
loop per client; each iteration awaits a few short tasks. Replaying the bench's
frame trace (4 clients, ~10k frames before the old bump arena overflowed):

| allocator | peak frame land |
| --- | --- |
| bump only (before) | 4.2 MB, overflowed the 4 MiB default |
| ring, retiring dead prefixes and a dead tail | 4.2 MB |
| bump + top retraction + exact-size free lists | 12.6 KB |
| live frames at any instant (lower bound) | 10.2 KB, 23 frames |

The worker loops are allocated first and never die, so in ring order they sit
at the head and pin it: no prefix ever retires, and the ring cannot wrap past
them. That is not a bench quirk but the ordinary shape of structured work, a
long-lived child that keeps awaiting short ones.

[`firm_frame_arena`](../../src/nxtrt/task/frame_arena.hpp) is therefore:

- bump allocation from the top of borrowed land or nonmoving owned chunks;
- a freed frame on top retracts the top (stack discipline for inline awaits);
- any other freed frame joins a free list for its exact block size, and the
  next frame of that size reuses it. Coroutine frames come in very few sizes,
  one per coroutine function (9 in the bench), so exact sizes almost always
  match. The list is intrusive, living in the dead frame itself;
- up to 16 size classes; a freed block with no free class is stranded until
  reset and reported as such;
- the whole arena resets when its last frame dies.

Allocation never suspends. Borrowed land never falls back to heap growth;
owned mode deliberately acquires chunks as needed. Borrowed exhaustion throws
a `runtime_error` naming the firm, frame and block size, alignment, live
bytes and frames, top, high-water mark, free-listed and stranded bytes, and
the task that was creating the frame. With reuse, 240k frames in the bench run
fit in 13.9 KB.

A firm cannot be moved once a frame lives in its land, because frame headers
name their arena; the firm move constructor aborts if that happens.

## Invariants

A task frame allocated from firm land may not outlive that firm.

A block can be reused after its frame is safely destroyed, independently of
other live frames. The arena as a whole cannot disappear while any frame still
uses it. That includes directly awaited tasks and pool jobs, not only forked
firm children. Cancellation is a request, not proof of settlement: outstanding
backend references must be drained before their storage can be released.

A frame pointer stored in the deck task registry names memory owned by a firm,
not memory owned by the deck. The deck can identify and schedule the task, but
it does not become the allocator for the frame.

Tasks created outside a firm are invalid. If a caller wants root work, it must
enter a root firm first. This establishes a frame provider, not automatically a
firm child record for every coroutine.

## API Direction

Prefer task factories over preconstructed tasks when forking:

```cpp
auto child = fork(fn, args...);
```

instead of only:

```cpp
auto child = fork(fn(args...));
```

That postpones allocation until invocation in an ambient frame scope. The
current member `firm::fork(fn, args...)` does not itself rebind the ambient
firm or retain the callable. Merely passing a factory therefore does not cure
the capturing-coroutine-lambda lifetime hazard.

Tuple helpers retain their factories, and pools put ideas in stable slots
before invoking them. A preconstructed task retains its original allocation
home regardless of which owner later admits it. Recipe-based APIs make this
decision explicit without claiming that every callable lifetime is automatic.

## Relationship To Other RFCs

[RFC 0003](rfc-0003-deck-task-registry.md) gives tasks deck-owned identities
and registry rows. Much control state remains in promises. This RFC supplies
the memory side: frame residence is independent of registry identity and of
the owner that holds the task handle.

[RFC 0013](rfc-0013-runtime-env-core-fields.md) provides the hot `current_firm`
field needed by promise allocation.

[RFC 0010](../new/rfc-0010-firm-buffer-groups-and-io-land.md) generalizes the same
territory idea from coroutine frames to I/O buffers.

## Open Questions

- ~~What is the smallest ring-shaped frame allocator that can support aligned
  coroutine frames and prefix retirement?~~ A ring does not fit frame
  lifetimes; see the implementation section.
- ~~Do completed child frames get reused before firm settlement?~~ Yes: a
  frame's land is reusable as soon as that frame is destroyed.
- Should frame exhaustion become backpressure instead of an error? Recipes
  allow waiting before frame construction, but pool slot admission does not
  currently implement a frame-byte budget. `farm::alloc()` reports exhaustion;
  it does not wait for a slot.
- How should frame provision be separated from nursery ownership? Root entry
  currently installs a firm; pools still rely on that allocation scope.
- Which additional pressure metrics are useful beyond the current live-frame,
  high-water, free-listed and stranded-byte diagnostics?
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
