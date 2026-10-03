# nxtrt runtime overview {#rt_overview}

`nxtrt` is the experimental async runtime used by `src`. It is a small
coroutine runtime with explicit scheduling, lifetime scopes, and a backend
boundary for platform I/O.

The public namespace currently contains several abstraction levels at once:
high-level task composition, the scheduler that drives tasks, structured
concurrency groups, buffered byte streams, and low-level backend operations.
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
tasks, pools, and continuations remain confined to the original deck.

## Decks {#rt_deck}

A deck is the cooperative scheduler for the runtime. It owns a ready queue and
resumes tasks in pump rounds.

`run_ready()` is intentionally one round: it resumes the handles that were
ready at the start of the call. Tasks that become ready during the call are
left for a later round. This keeps scheduling explicit and avoids surprising
reentrancy.

`sync_wait(fn, args...)` is the bridge from synchronous code into the runtime:
it calls the task factory inside the deck's runtime environment, keeps the
factory alive while the root task runs, and pumps the deck until that task
completes. `runtime::run`, `run_with_kqueue`, and the io_uring `nxtrt::run`
enter the same way; there is no enclosing scope object around the root task.
A deck may also be paired with a @ref rt_wand "wand" so tasks can await
external I/O.

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

`on_completed` registers a synchronous, non-suspending callback without
starting the task or consuming its result:

```cpp
auto link = child.on_completed([&]() noexcept {
    // Inspect child.result(), update local state, or queue a continuation.
});
```

The returned `completion_link` owns the callable but borrows the task. Keep it
alive until notification; destruction or `disconnect()` detaches it. Moving
either the link or the task preserves the registration. Destroying the task
disconnects the link without notifying it. Only one completion observer may be
connected at a time; the ordinary awaiting continuation is independent.

Callbacks must return `void` and be `noexcept`. They run at final suspension,
in the completing task's context, before its awaiting continuation is queued.
Registering on an already-completed task instead calls back immediately in the
registering context. A callback must not destroy the completing task or resume
another coroutine inline: queue continuations so final suspension can finish.

Concrete API:

- @ref nxtrt::task "nxtrt::task<T>"
- @ref nxtrt::completion_link "nxtrt::completion_link<Fn>"
- @ref nxtrt::task_id "nxtrt::task_id"

## Groups {#rt_group}

Fixed concurrency is a group of tasks awaited by a task. An idea is a
callable that returns `task<T>` (or `hope<T>`) when invoked; a task is work
that has already been created. A group directly owns its tasks in a tuple or
vector, observes completion through stable observers, and drains every started
task before the awaiting task resumes. It has no pool backing, recipe wrappers,
or separate intermediate results tuple. There is no fork, join, or public deed:
a group owns exactly the tasks it was given, and they cannot outlive it.

`settle(std::tuple{tasks...}, execution)` runs a fixed heterogeneous set of tasks,
owned directly in the tuple. Tasks are created before entering the group; their
coroutine bodies begin when they start. Factories are not accepted. The
result is `std::tuple<outcome<T>...>` in tuple order, where `outcome<T>` is
`std::expected<T, std::exception_ptr>` (including `outcome<void>`); a job
stopped before it started settles as cancelled. `settle_range(range, execution)` does
the same for a homogeneous range and returns `std::vector<outcome<T>>` in range
order.

The policy predicate decides, as each job settles, whether to stop the rest:

- `all_group` (the default) lets every job finish;
- `fail_fast_group` and `first_success_group` stop the others on the first failure
  or success;
- `first_completion_group` stops the others when any job settles;
- `primary_group` stops the companions when job 0, the primary, settles.

Policies are ordinary callables with signature
`bool(std::size_t index, bool failed) noexcept`, passed by value. For example,
`settle(std::tuple{std::move(work), std::move(watcher)}, primary_group{})`, or
use a lambda to stop on a configured index:

```cpp
settle(std::move(tasks), [index](std::size_t settled, bool) noexcept {
    return settled == index;
});
```

Each child has a synchronous completion link that consults the policy and
counts down. The combiner awaits that countdown before returning; reaching
zero queues it for a later deck turn, never resumes it inside final suspension.
There are no group objects, wrapper coroutines, or extra per-child deck slots.
Policy notification runs in the completing child's context, during final
suspension, and requests stop directly on unfinished siblings. Cancellation
is still cooperative: a stopped child can continue until it observes stop.

Policies see the task promise's success or failure without moving its value.
Results stay in their promises until all started tasks have drained, then move
into the returned outcomes. Initial extraction errors become exception outcomes
without changing the group's stopping decision. Subsequent moves while constructing
or delivering the result tuple/vector can still throw; children are already
drained at that point.

A stop chosen by the policy is a normal finish. Outside cancellation stops the
group's tasks and drains them before propagating cancellation.

The usual helpers are written over `settle`. `when_all(tuple)` /
`when_all(tasks...)` and `when_all_range` return every value in order (void
positions are `std::monostate`), stopping the rest and rethrowing the first
failure to complete, never the `operation_cancelled` of a task it stopped. `wait_any(tuple)` / `wait_any(tasks...)` and `wait_any_range` return
the first success in input order, not first completion, stop the rest on
success, and group the failures if none succeeds. `with_timeout` and
`poll_until_after` use the same route. See the
[implemented tuple contract](../rfc/new/rfc-0014-idea-algebra.md#implemented-task-tuples).

```cpp
auto [page, icon] = co_await nxtrt::settle(
    std::tuple{
        fetch(page_url),
        fetch(icon_url)});
if (!page)
    std::rethrow_exception(page.error());
```

Cancellation belongs to tasks. Stop propagates from an awaiting task to the
task it awaits, and a group stops its own directly owned tasks.
`current_stop_token()`, `stop_requested()`, and `throw_if_stop_requested()`
read the running task's stop state.

`nxtrt/idea.hpp` names the broader recipe constraint: `idea<Fn>` is a
move-constructible callable invoked as a mutable stored lvalue, returning
`task<T>` or `hope<T>` by value. `idea_result_t<Fn>` names `T`, and
`idea_of<Fn, T>` checks it. An idea is not a `std::function` or an erased owning
wrapper. Consumers invoke each admitted recipe once and preserve its storage
through settlement, including failure and cancellation; the concept itself
cannot enforce those obligations. Hope-producing ideas may complete without
allocating a coroutine. Tuple and range groups accept tasks only; call factories
before entering a group and keep any borrowed factory state alive yourself.

Concrete API:

- @ref nxtrt::settle "nxtrt::settle"
- @ref nxtrt::settle_range "nxtrt::settle_range"
- @ref nxtrt::when_all "nxtrt::when_all"
- @ref nxtrt::wait_any "nxtrt::wait_any"

## Pools {#rt_pool_overview}

A [bounded idea pool](rt-pool.md) turns a homogeneous feed of recipes into a
completion-order result feed. It borrows farm slots and output land; consuming
results returns admission capacity. Pool jobs are owned directly by their
slots. Ready hopes stay synchronous, while pending tasks use the existing deck.
Unlike fixed groups, pools are streaming, bounded idea-factory evaluators.

`drain(ideas, capacity)` runs a feed of ideas through a pool, at most
`capacity` at once, and discards the results; the first failure stops
admission, cancels and drains the running jobs, and is rethrown. Callers that
want the results construct a `pool<Idea>` over a `pool_land<Idea>{capacity}`,
which owns the slots and output cells.

See [Recipes, pools, and structured async](rt-concurrency-direction.md) for
the design direction: teams versus pools, outcomes, lifetime-aware terminal
consumption, and the separate Wisp operation-awaiting bridge.

Concrete API:

- @ref nxtrt::pool "nxtrt::pool<Idea>"
- @ref nxtrt::pool_land "nxtrt::pool_land<Idea>"
- @ref nxtrt::drain "nxtrt::drain"

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
