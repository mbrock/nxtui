# nxtrt runtime overview {#rt_overview}

`nxtrt` is the experimental async runtime used by `src`. It is a small
coroutine runtime with explicit scheduling, lifetime scopes, and a backend
boundary for platform I/O.

The public namespace currently contains several abstraction levels at once:
high-level task composition, the scheduler that drives tasks, structured
concurrency handles, buffered byte streams, and low-level backend operations.
This page is the conceptual map for those pieces. The class and function pages
remain the exact reference for the corresponding C++ declarations.

## Execution model {#rt_execution_model}

The core value is @ref nxtrt::task "task<T>": a movable owning handle to a
lazy coroutine frame. Awaiting or scheduling it does not itself transfer
ownership; moving or releasing the handle does.

Tasks do not run on construction. They run when a @ref nxtrt::deck "deck" puts
their coroutine handle on its ready queue and later resumes that handle.
Completion also returns to the deck: a task that finishes wakes its
continuation by enqueueing it, rather than by resuming it inline.

Ordinary blocking C++ can run off the deck through @ref rt_blocking
"blocking pools". Only owned callables and outcomes cross that boundary;
tasks, firms, and continuations remain confined to the original deck.

## Decks {#rt_deck}

A deck is the cooperative scheduler for the runtime. It owns a ready queue and
resumes tasks in pump rounds.

`run_ready()` is intentionally one round: it resumes the handles that were
ready at the start of the call. Tasks that become ready during the call are
left for a later round. This keeps scheduling explicit and avoids surprising
reentrancy.

`sync_wait()` is the bridge from synchronous code into the runtime: it starts
a root task and pumps the deck until that task completes. A deck may also be
paired with a @ref rt_wand "wand" so tasks can await external I/O.

Concrete API:

- @ref nxtrt::deck "nxtrt::deck"
- @ref nxtrt::yield "nxtrt::yield()"

## Tasks {#rt_task}

A task is both a coroutine return type and a movable handle to the coroutine
frame. Its promise stores the result or exception, stop state, continuation,
runtime environment, and the task id used for tracing and ambient observation.

Awaiting a task connects child to parent. The awaited task is enqueued on the
active deck, and the awaiting task becomes its continuation. When the child
reaches final suspend, the parent is requeued for a future pump step.

Concrete API:

- @ref nxtrt::task "nxtrt::task<T>"
- @ref nxtrt::task_id "nxtrt::task_id"

## Firms {#rt_firm}

Firms are lifetime scopes: they provide coroutine-frame memory and cancellation
context, and may optionally own explicitly forked children. A firm is not
synonymous with a nursery; scope-only work needs no child records or join.

`with_firm<Policy = firm>(fn)` retains a nullary factory for scope-only work,
or accepts a factory taking `Policy&` when explicit child ownership is needed.
That scope reference provides `fork`, `join`, and `stop`. There are no free
`nxtrt::fork` or `nxtrt::join` functions, and tasks do not spawn ambiently into
the current firm. Pass the scope reference explicitly to nested work that
needs to fork. Call `scope.join()` before borrowed locals go out of scope;
leaving the body does not make it safe for children to keep using those locals.

Use `scope.fork(factory, args...)` to invoke work in the owner's frame context.
Preconstructed task frames must already be allocated by that owner or an
enclosing ancestor; a shorter-lived inner allocation scope is unsafe. The
ambient `current_firm` / `require_current_firm()` context remains available
for frame, cancellation, and debugging context, not implicit child admission.
Firm subclasses remain awaitable.

Higher-level helpers such as `when_all`, `wait_any`, and `with_timeout` are
written in terms of firms. They are not separate schedulers; they are
composition patterns over the same task and deck machinery.

For a fixed heterogeneous batch, pass a tuple of tasks or nullary task
factories:
`when_all(std::tuple{f, g})`, `wait_any(std::tuple{f, g})`, or
`with_firm<Policy>(std::tuple{f, g})`. This does not make the firm a fixed
child-ownership container: tuple main work is lowered to finite indexed
`task<void>` recipes in the existing pool, with typed settled outcomes stored
at their tuple positions and no child records or deeds.
The firm supplies frames and stop policy; separately forked children require
an explicit scope reference and are outside the tuple's fixed bound.
`with_firm<Policy>(tuple)` returns `expected<T, exception_ptr>` outcomes
(including `expected<void, ...>`), not `catching_deed` handles. `when_all` and
`wait_any` preserve their value, cancellation/drain, and input-order selection
contracts; variadic forms delegate to tuple forms. `with_timeout` and
`poll_until_after` use the same route. See the
[implemented tuple contract](../rfc/new/rfc-0014-idea-algebra.md#implemented-task-tuples).

`nxtrt/idea.hpp` names the broader recipe constraint: `idea<Fn>` is a
move-constructible callable invoked as a mutable stored lvalue, returning
`task<T>` or `hope<T>` by value. `idea_result_t<Fn>` names `T`, and
`idea_of<Fn, T>` checks it. An idea is not a `std::function` or an erased owning
wrapper. Consumers invoke each admitted recipe once and preserve its storage
through settlement, including failure and cancellation; the concept itself
cannot enforce those obligations. Hope-producing ideas may complete without
allocating a coroutine. This does not extend the tuple helpers' task-only
factory contract or turn firms into a future fixed-team abstraction.

Concrete API:

- @ref nxtrt::firm "nxtrt::firm"

## Pools {#rt_pool_overview}

A [bounded idea pool](rt-pool.md) turns a homogeneous feed of recipes into a
completion-order result feed. It borrows farm slots and output land; consuming
results returns admission capacity. Pool jobs are owned directly, not retained
as firm child records. Ready hopes stay synchronous, while pending tasks use
the existing deck.

See [Recipes, pools, and structured async](rt-concurrency-direction.md) for
the design direction: teams versus pools, explicit coping, lifetime-aware
terminal consumption, and the separate Wisp operation-awaiting bridge.

## Deeds {#rt_deed}

A deed is the caller's handle to a task forked into a firm. It is deliberately
not the same thing as a task: the firm owns and joins the child work, while
the deed lets user code recover the child's result after the firm has reached
the appropriate point.

`deed<T>` rethrows child failure when read. Moving it through `.cope()` explicitly
selects `catching_deed<T>`, whose `get()` returns an expected-like outcome so
dynamic-fork users can collect child outcomes before deciding what to return
or throw. Fixed tuple composition instead returns settled expected outcomes
directly; it does not make deeds for its pool-owned main work.

Concrete API:

- @ref nxtrt::deed "nxtrt::deed<T>"
- @ref nxtrt::catching_deed "nxtrt::catching_deed<T>"

## Wishes {#rt_wish}

A wish is an awaitable request for outside work. It is a closed operation
value: read some bytes, write some bytes, wait for readiness, open a file,
wait for a timeout, and so on.

When task code awaits a wish, the active deck asks its active wand to prepare
that operation. Preparation returns a typed urge, and the urge parks the
current coroutine until the backend fulfills or cancels the operation.

This split keeps task code platform-neutral. The task names what it wants; the
wand decides how to stage and complete that work on a particular platform.

Concrete API:

- @ref nxtrt::op "nxtrt::op"
- @ref nxtrt::urge "nxtrt::urge<T>"
- @ref nxtrt::need "nxtrt::need"

## Wands {#rt_wand}

A wand is the backend boundary. It receives prepared wishes, stores parked
tasks, submits platform work, and later resumes tasks by putting them back on
a deck.

`wave()` is called after a deck pump round. That gives task code a chance to
stage several operations synchronously, then lets the wand submit them as a
batch or in whatever order the backend needs.

Current concrete wands live at the implementation edge:

- @ref nxtrt::wand "nxtrt::wand"
- `uring_wand`
- `kqueue_wand`

## Byte streams and protocol helpers {#rt_io}

The runtime also contains reusable async I/O utilities. Byte readers own the
buffered hot path and expose small `hope<T>` read operations; concrete readers
override the refill edge. Byte writers mirror that shape for buffered writes,
with concrete writers overriding the drain edge.

Protocol helpers such as `nxtrt::fs` and `nxtrt::http` sit above the wish and
byte-stream layers: they use runtime I/O, but they are not scheduler
primitives.

Concrete API:

- @ref nxtrt::bytefeed "nxtrt::bytefeed"
- @ref nxtrt::bytesink "nxtrt::bytesink"
- @ref nxtrt::fs "nxtrt::fs"
- @ref nxtrt::http "nxtrt::http"
