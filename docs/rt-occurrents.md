# Behavioral threads as occurrent structure {#rt_occurrents}

This note sits beside @ref rt_holding and @ref rt_game. It is a place to keep
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
| `firm` | a **place** — a region of memory owning sub-objects, `located-in` a spatial region | its **history**: the structured unfolding of its children |
| `deed<T>` | the handle a parent holds | (its referent) the child occurrent it observes |
| forked child task | the child `task` value | an occurrent part of the parent process |
| `wish` | a **realizable** — a disposition to do I/O | — (not an occurrent; it is *realized in* an exec) |
| `exec` | the backend record and its lifecycle state | the process that realizes the wish |
| cancellation | — | a control happening forcing a terminating subhistory |
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
When a firm exits, its child deeds are not allowed to drift away because those
child tasks are occurrent parts of the firm's **history**. But the firm itself
is not that history — it is the *place* the history happens, which turns out to
matter enough to take up its own section below.

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
- a **terminal boundary** — cancellation; the history is forced to end early,
  before it ever reaches its completion boundary.

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

Cancellation is the cleanest case, and it is why "boundary" has to mean more
than "instant." When a firm stops, that is one terminal boundary in the firm's
history — but because the children are occurrent parts of that history, it
imposes a terminal boundary on each child's history as well. The
`operation_cancelled` that propagates up the stack *is* that boundary sweeping
along the `occurrent-part-of` relation, synchronously, with no trace-time
between the links. So a terminal-boundary cascade (cancellation) and a
synchronous phase-transition cascade (the hot path) are the *same shape*: a
chain of process-boundaries with no process between them. Cancellation is just
the cascade whose links are endings.

This even reframes efficiency in occurrent terms, and ties the note to
@ref rt_holding. A genuine process — a wait — costs a deck round-trip and a
parked frame. A boundary-cascade costs almost nothing. So "make the buffered
case free" is precisely *turning a process into a boundary*: a `read_some` that
would have been a wait becomes, on warm data, one more synchronous transition
in a cascade. A `hope` that must fall back to a `task<T>` is a real process,
with an interior boundary at each suspension; a `hope` that is `ready` is only
an edge. The `eager-wand` endgame is the same wish stated in general — collapse
every avoidable wait into an edge.

## The firm is a place, not a process {#rt_occurrents_firm}

It would be a mistake to read the whole runtime as occurrents; pushed too far
the lens distorts, and the `firm` is where it distorts first. The dictionary
lists a firm-*history* in the occurrent column, but the firm **itself** belongs
in the other column — and, more pointedly, it is *spatial*. A firm is a region
of memory that owns sub-objects: its storage sits at some range of addresses,
which is to say it is `located-in` a spatial region. That is BFO's own axiom
[134-001], already in `bfo-sketch.rkt` — every independent continuant is
located in some spatial region at every time. The firm's bytes are, quite
literally, somewhere inside your computer.

And here the ontology says something almost uncanny: a region of memory really
*is* a region of space. The address range resolves to physical cells in a
memory chip, which occupy actual three-dimensional volume. Swept through the
firm's lifetime, that spatial region traces out a **spatiotemporal region** —
BFO's occurrent-side counterpart to the spatial one. So the firm wears both
faces at once: it *is* a spatial continuant, and it *has* a history that
occupies a spatiotemporal region. The history — the structured unfolding of its
children — is the occurrent. The firm is not that history; it is the place the
history happens.

This is the stronger reading of structured concurrency. The firm is a container
(a place); its children are sub-objects located within it; and the single rule
that makes it *structured* is a constraint binding the **continuant** to its
**occurrent**: the firm's storage may not be reclaimed — the place may not be
torn down — until every child's history has reached a terminal boundary
(settled, joined, or cancelled). RAII gives you the place and the moment of its
destruction; structured concurrency forbids that destruction until the
histories inside the place have closed. The scope is spatial; the discipline is
the leash between the space and the time.

## What this suggests for the model {#rt_occurrents_model}

The `rdf-forge` work points at a useful split:

- Export RDF/OWL vocabulary for the ontology of runtime things.
- Keep BFO-style CLIF/FOL annotations where the exact logical shape matters.
- Generate executable Forge constraints for the fragments that fit the current
  relational model.

For runtime semantics, the same split suggests a path:

- split each runtime word into its continuant/occurrent pair, and classify the
  *histories* of `TASK`, `EXEC`, and `DEED` as process-like — while `FIRM` and
  `GAME` are **places** (spatial continuants) whose histories are the
  occurrents, and `WISH` stays a **realizable** continuant, `realized-in` its
  `exec`;
- model `spawned`, `issued`, and `has-continuation` as occurrent-part relations,
  and the role of an fd, a buffer, or a `coin` as `participates-in` — a
  continuant taking part in a process, not a part of it;
- treat lifecycle states and sync points as boundaries or phases rather than
  as ordinary object fields;
- express structured concurrency as closure over occurrent parts: child work
  owned by a scope must settle, be cancelled, or be joined before the scope can
  retire;
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

