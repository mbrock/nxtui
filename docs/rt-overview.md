# nxt::rt runtime overview {#rt_overview}

`nxt::rt` is the experimental async runtime used by `src`. It is a small
coroutine runtime with explicit scheduling, structured child tasks, and a
backend boundary for platform I/O.

The public namespace currently contains several abstraction levels at once:
high-level task composition, the scheduler that drives tasks, structured
concurrency handles, buffered byte streams, and low-level backend operations.
This page is the conceptual map for those pieces. The class and function pages
remain the exact reference for the corresponding C++ declarations.

## Execution model {#rt_execution_model}

The core value is @ref nxt::rt::task "task<T>": a lazy coroutine frame that
uniquely owns its state until it is moved into another task, awaited by a
parent task, forked into a zone, or driven by a deck.

Tasks do not run on construction. They run when a @ref nxt::rt::deck "deck"
puts their coroutine handle on its ready queue and later resumes that handle.
Completion also returns to the deck: a task that finishes wakes its continuation
by enqueueing it, rather than by resuming it inline.

## Decks {#rt_deck}

A deck is the cooperative scheduler for the runtime. It owns a ready queue and
resumes tasks in pump rounds.

`run_ready()` is intentionally one round: it resumes the handles that were
ready at the start of the call. Tasks that become ready during the call are left
for a later round. This keeps scheduling explicit and avoids surprising
reentrancy.

`sync_wait()` is the bridge from synchronous code into the runtime: it starts a
root task and pumps the deck until that task completes. A deck may also be
paired with a @ref rt_wand "wand" so tasks can await external I/O.

Concrete API:

- @ref nxt::rt::deck "nxt::rt::deck"
- @ref nxt::rt::yield "nxt::rt::yield()"

## Tasks {#rt_task}

A task is both a coroutine return type and a movable handle to the coroutine
frame. Its promise stores the result or exception, stop state, continuation,
runtime environment, and the task id used for tracing and ambient observation.

Awaiting a task connects child to parent. The awaited task is enqueued on the
active deck, and the awaiting task becomes its continuation. When the child
reaches final suspend, the parent is requeued for a future pump step.

Concrete API:

- @ref nxt::rt::task "nxt::rt::task<T>"
- @ref nxt::rt::task_id "nxt::rt::task_id"

## Zones {#rt_zone}

Zones provide structured concurrency. A zone is an extent in which tasks can be
forked, joined, stopped together, and observed after the zone has reached a
join point.

Forking a task into a zone starts child work without immediately awaiting it.
The zone keeps enough shared state to stop children, join them, and surface
their results or exceptions in a controlled order.

Higher-level helpers such as `when_all`, `wait_any`, and `with_timeout` are
written in terms of zones. They are not separate schedulers; they are
composition patterns over the same task and deck machinery.

Concrete API:

- @ref nxt::rt::task_zone "nxt::rt::task_zone"

## Deeds {#rt_deed}

A deed is the caller's handle to a task forked into a zone. It is deliberately
not the same thing as a task: the zone owns and joins the child work, while the
deed lets user code recover the child's result after the zone has reached the
appropriate point.

`deed<T>` rethrows child failure when read. `catching_deed<T>` carries an
expected-like result so helpers can collect multiple child outcomes before
deciding what to return or throw.

Concrete API:

- @ref nxt::rt::deed "nxt::rt::deed<T>"
- @ref nxt::rt::catching_deed "nxt::rt::catching_deed<T>"

## Wishes {#rt_wish}

A wish is an awaitable request for outside work. It is a closed operation value:
read some bytes, write some bytes, wait for readiness, open a file, wait for a
timeout, and so on.

When task code awaits a wish, the active deck asks its active wand to prepare
that operation. Preparation returns a typed waiter, and the waiter parks the
current coroutine until the backend fulfills or cancels the operation.

This split keeps task code platform-neutral. The task names what it wants; the
wand decides how to stage and complete that work on a particular platform.

Concrete API:

- @ref nxt::rt::op "nxt::rt::op"
- @ref nxt::rt::waiter "nxt::rt::waiter<T>"
- @ref nxt::rt::parked_task "nxt::rt::parked_task"

## Wands {#rt_wand}

A wand is the backend boundary. It receives prepared wishes, stores parked
tasks, submits platform work, and later resumes tasks by putting them back on a
deck.

`wave()` is called after a deck pump round. That gives task code a chance to
stage several operations synchronously, then lets the wand submit them as a
batch or in whatever order the backend needs.

Current concrete wands live at the implementation edge:

- @ref nxt::rt::wand "nxt::rt::wand"
- `uring_wand`
- `kqueue_wand`

## Byte streams and protocol helpers {#rt_io}

The runtime also contains reusable async I/O utilities. Byte sources and sinks
are the runtime-polymorphic boundary. Readers, writers, and pipes are
composition tools built on top of tasks.

Protocol helpers such as `nxt::rt::fs` and `nxt::rt::http` sit above the wish
and byte-stream layers: they use runtime I/O, but they are not scheduler
primitives.

Concrete API:

- @ref nxt::rt::byte_source "nxt::rt::byte_source"
- @ref nxt::rt::byte_sink "nxt::rt::byte_sink"
- @ref nxt::rt::byte_reader "nxt::rt::byte_reader"
- @ref nxt::rt::byte_writer "nxt::rt::byte_writer"
- @ref nxt::rt::pipe "nxt::rt::pipe<T>"
- @ref nxt::rt::fs "nxt::rt::fs"
- @ref nxt::rt::http "nxt::rt::http"
