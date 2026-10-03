# Behavioral threads as occurrent structure {#rt_occurrents}

The runtime's behavioral-programming coordinator, `game<Event>`, has been
removed, and so have firms and deeds; concurrent work is now a group of ideas
awaited by a task, run in a pool. This note keeps the ideas those types
explored and says, where it matters, what the code does now.

This note sits beside @ref rt_holding. It is a place to keep
one of the stranger and more promising ideas in the runtime model: behavioral
threads, coroutines, and structured concurrency look less like a pile of
control-flow tricks when they are read as a small logic of **occurrents**.

That word is borrowed from BFO. A continuant is something like an object that
persists through time. An occurrent is something that unfolds: a process, an
event, a history, a region of happening. BFO holds the two **disjoint** —
nothing is both at once (`disjoint continuant occurrent`). The runtime is full
of the second kind, but it usually spells them with the same word as the first.
A `task<T>` *value* is a continuant: a coroutine frame you own, move, and store.
The *task* it stands for is an occurrent: work unfolding in time, with pauses,
boundaries, children, dependencies, and an end. These are two entities, not
one wearing two hats — and almost every noun below is really such a pair, a
continuant handle onto a happening.

For the current implementation boundaries behind this ontology, see
[Runtime concurrency direction](rt-concurrency-direction.md). The metaphors
below do not identify physical frame allocation with structured child ownership.

That makes behavioral programming feel less like an extra coordination API and
more like a visible fragment of the runtime's ontology. A b-thread is an
occurrent whose public behavior is a sequence of sync points. A game is a
processual context in which those b-threads overlap. A selected event is not
"called" by any one thread; it is the next admissible happening in the shared
context.

## The tempting dictionary {#rt_occurrents_dictionary}

The correspondences are not exact definitions yet, but they are good handles.
Because continuant and occurrent are disjoint, the honest table has two
columns: the *thing* the runtime holds, and the *happening* it is a handle
onto. Some words are purely one or the other (a `—` marks the absent side):

| Runtime word | Continuant (the thing held) | Occurrent (the happening) |
| --- | --- | --- |
| `task<T>` | the coroutine frame you own and move | the work unfolding — a process, at first only intended |
| `deck` | the scheduler object and its ready queue | a deck round / pump — boundary-like |
| coroutine suspension | — | a boundary between phases of the task-process |
| final suspend | — | the boundary where the task-process completes |
| `pool` | a **place** — slots and output cells, `located-in` a spatial region | its **history**: the admissions and settlements of its jobs |
| group (`settle`, `when_all`) | the outcome tuple or vector the awaiting task receives | the overlapping jobs, an occurrent part of the awaiting task's process |
| group job | the job's `task` value, held in a pool slot | an occurrent part of the group |
| `wish` | a **realizable** — a disposition to do I/O | — (not an occurrent; it is *realized in* an exec) |
| `exec` | the backend record and its lifecycle state | the process that realizes the wish |
| cancellation | — | a stop request followed by cooperative termination and any required drain |
| `game<Event>` | the game coordinator object | the processual context the b-threads share |
| b-thread | the `task` value | an occurrent part of the game-process |
| selected event | — | a shared happening admitted by the context |
| `post` / `wait` / `halt` | — | modal stances toward the next happening |

The `wish` row is the one that earns its keep. A wish is not a small process;
it is a **realizable entity** — a disposition to do I/O — and the runtime even
says so: `runtime.rkt` declares `exec realizes one wish`. So the occurrent is
the `exec`, and `realizes` is BFO's realization relation pointing from that
process to the disposition it discharges. The wish never enters the occurrent
column at all.

This is why temporal logic keeps appearing in the same room as structured
concurrency. A runtime trace is a bounded history of occurrents. A safety
property says some kind of bad happening is never part of any admissible
history. A liveness property says some suspended process part must eventually
reach a later boundary.

## Occurrent parthood is not just slicing time {#rt_occurrents_parthood}

The important caution: occurrent parthood is not merely a temporal partition.
It is tempting to say that a process is made of "first this time interval, then
that one." Sometimes that is useful, but it is much too thin.

A b-thread can be part of a game for the same span as other b-threads. A child
task can overlap its parent rather than occupying a neat before-or-after
segment. An HTTP request task may have an I/O wait as one part, a parser as
another, and a cancellation race as another; those parts are structural and
causal as much as temporal. They are not just consecutive slices of a line.

BFO already draws this line, and `bfo-sketch.rkt` encodes it: `temporal-part-of`
is a *proper subproperty* of `occurrent-part-of`. The neat before/then segments
are the temporal parts; the overlapping, structural, causal parts are occurrent
parts that are **not** temporal parts. The runtime lives in the gap between the
two relations, and structured concurrency is the discipline that manages that
gap. The continuants in play — an fd, the bytes in a buffer, a `coin` — are not
parts of the process at all; they `participate-in` it, which is why
`bfo-sketch.rkt` now carries a participation relation alongside parthood.

That distinction matters for the runtime. Structured concurrency is a claim
about **ownership of occurrent parts**, not just about start and stop times.
Before a group returns, every one of its jobs must settle, because those jobs
are occurrent parts of the awaiting task's process. The outcomes it returns are
continuants that record how each part ended; they survive settlement, and the
executions do not. The pool that holds the jobs is not that history either — it
is the *place* the history happens, which turns out to matter enough to take up
its own section below. (The removed `firm` was an earlier attempt at the same
place, and `deed<T>` an earlier attempt at the outcome.)

Behavioral threads sharpen the same point. A game super-step is not one
b-thread's private next moment. It is a shared event selected from many
overlapping declarations. The b-threads are simultaneous process parts whose
constraints combine. The "next" event belongs to the whole game context.

## Kinds of boundary: completion, suspension, cancellation {#rt_occurrents_boundaries}

BFO's `process-boundary` is a temporal part of a process with *no proper
temporal parts of its own* — instantaneous, all edge and no interior. Once you
have that notion, the right question is which runtime happenings are really
processes and which are only boundaries, and the answer is surprising: most of
what the runtime *does* is boundaries.

They are not all the same boundary, though. At least three kinds matter, and
the runtime treats them differently:

- a **completion boundary** — final suspend; the process reaches its intended
  end and its history closes;
- a **suspension boundary** — an interior edge; the task-process pauses at a
  `co_await` and *continues* afterward, so the boundary sits between two parts
  of one ongoing history;
- a **terminal boundary** — eventual termination after cancellation; a stop
  request alone is not that boundary.

Now the sharp part, and the reason to take synchronous phase transitions
seriously. A `hope<T>` that is already `ready`, an `exec` stepping `prepared →
parked`, a `wave()` flushing staged wishes, a game super-step selecting and
publishing an event — none of these waits. Each is a boundary, and they
**chain**: within a single synchronous resume the runtime can cross a whole run
of them with no process in between. Such a boundary-cascade occupies *no
trace-time at all*. The only genuine processes — the only happenings with an
interior, that really take up trace-time — are the **waits**: a live I/O wait,
a timer, a task parked for an event that has not arrived. Everything else is
edges between edges.

Cancellation exposes the limit of the boundary metaphor. A stop request
propagates synchronously from a task to the task it awaits, and from a group
to its jobs, but their actual completion is cooperative. Observing stop,
unwinding with `operation_cancelled`, and draining backend operations can
require later deck turns and backend events.
The request cascade is not a cascade of completed histories: stop is control,
settlement is an outcome, and backend retirement may still require drain.

This even reframes efficiency in occurrent terms, and ties the note to
@ref rt_holding. A genuine process — a wait — costs a deck round-trip and a
parked frame. A boundary-cascade costs almost nothing. So "make the buffered
case free" is precisely *turning a process into a boundary*: a `read_some` that
would have been a wait becomes, on warm data, one more synchronous transition
in a cascade. A `hope` that must fall back to a `task<T>` is a real process,
with an interior boundary at each suspension; a `hope` that is `ready` is only
an edge. The `eager-wand` endgame is the same wish stated in general — collapse
every avoidable wait into an edge.

## The pool is a place, not a process {#rt_occurrents_pool}

It would be a mistake to read the whole runtime as occurrents; pushed too far
the lens distorts, and the holder of concurrent work is where it distorts
first. The runtime used to call that holder a `firm`; today it is a `pool` and
its land. The dictionary lists a pool-*history* in the occurrent column, but
the pool **itself** belongs in the other column — and, more pointedly, it is
*spatial*. A pool is a region of memory that owns sub-objects: its slots and
output cells sit at some range of addresses, which is to say it is
`located-in` a spatial region. That is BFO's own axiom [134-001], already in
`bfo-sketch.rkt` — every independent continuant is located in some spatial
region at every time. The pool's bytes are, quite literally, somewhere inside
your computer.

And here the ontology says something almost uncanny: a region of memory really
*is* a region of space. The address range resolves to physical cells in a
memory chip, which occupy actual three-dimensional volume. Swept through the
pool's lifetime, that spatial region traces out a **spatiotemporal region** —
BFO's occurrent-side counterpart to the spatial one. So the pool wears both
faces at once: it *is* a spatial continuant, and it *has* a history that
occupies a spatiotemporal region. The history — jobs admitted into slots,
running, settling, and leaving — is the occurrent. The pool is not that
history; it is the place the history happens.

The useful structured-concurrency rule is about owned executions: every job
must settle before its group returns and its [pool](rt-pool.md) is released.
Cancellation requests do not satisfy that rule by themselves; the group stops
its jobs and then drains them. Slots make the place literal: a slot is reused
only after the job that occupied it has settled and its outcome has been
consumed.

## What this suggests for the model {#rt_occurrents_model}

The `rdf-forge` work points at a useful split:

- Export RDF/OWL vocabulary for the ontology of runtime things.
- Keep BFO-style CLIF/FOL annotations where the exact logical shape matters.
- Generate executable Forge constraints for the fragments that fit the current
  relational model.

For runtime semantics, the same split suggests a path:

- split each runtime word into its continuant/occurrent pair, and classify the
  *histories* of `TASK` and `EXEC` as process-like — while `POOL` and its
  slots are **places** (spatial continuants) whose histories are the
  occurrents, and `WISH` stays a **realizable** continuant, `realized-in` its
  `exec`;
- model `admitted` and `has-continuation` as occurrent-part relations,
  and the role of an fd, a buffer, or a `coin` as `participates-in` — a
  continuant taking part in a process, not a part of it;
- treat lifecycle states and sync points as boundaries or phases rather than
  as ordinary object fields;
- express structured concurrency as closure over occurrent parts: child work
  admitted to a pool must settle before the pool can close, including completion
  of cancellation and any required backend drain;
- express behavioral programming as a temporal logic over a processual
  context: every super-step chooses one event that is requested and not
  blocked, and only matching b-threads advance.

The payoff would be a model where "coroutine correctness" is not only a list
of scheduler invariants. It becomes a logic of happening: what processes are
parts of what larger processes, what boundaries they may cross, what events
they may jointly admit, and when a structured region is allowed to end.

And the two halves of the project meet here. The `exec-state` lifecycle in
`runtime.rkt` is a continuant — the `exec` record — whose `has-lifecycle var
one exec-state` field *varies* across a trace. A continuant bearing a state
that changes over time is, in occurrent terms, a continuant participating in a
process whose **boundaries are the transitions**. So the `next-state` steps in
the temporal `lifecycle-transitions` predicate are not a separate formalism
bolted onto the ontology; they *are* the process-boundaries this note is
about. The Forge temporal model and the BFO occurrent reading are one
description of the same happening — which is the whole reason for wanting a
single language to write both.

