# RFC 0014: Idea Algebra {#rfc_idea_algebra}

Status: new

## Implementation status

The idea concept, task-tuple helpers, and bounded idea pool are implemented.
The general algebra, generic feed mapping, idea-level `cope`, channels, and
heterogeneous static teams remain proposals. The
[concurrency direction](../../docs/rt-concurrency-direction.md) connects these
pieces without treating the sketches below as shipped APIs.

## Summary

Task composition should mostly operate on recipes, not on already-born
`task<T>` values. A recipe may also return a `hope<T>`, keeping a synchronous
ready result free of coroutine allocation.

The implemented `nxtrt/idea.hpp` API names that recipe with a concept, not an
erased owning wrapper:

```cpp
idea<Fn>             // movable callable: Fn& -> task<T> or hope<T>, by value
idea_result_t<Fn>     // T
idea_of<Fn, T>        // idea<Fn> with exactly this eventual value type
```

`Fn` must be move-constructible and callable without arguments as a mutable
stored lvalue. Move-only and mutable closures qualify; rvalue-only call
operators do not. References to tasks/hopes, const-qualified return values,
other awaitables, and merely convertible result types do not qualify.
Invalid callable/result types fail the constraint rather than producing hard
trait errors. The distinction is:

```text
wish      = analyzable value recipe for outside work
idea<Fn>  = opaque callable recipe for a task<T> or hope<T>
task<T>   = already-born coroutine frame
```

An idea is a degenerate wish in one sense: it is a desire to compute `T`. But
unlike a wish, it is opaque. The runtime cannot inspect it until it is invoked
and a task or hope is produced.

Consumers invoke each admitted idea once and keep its stored callable alive
through settlement of the produced work, including failure and cancellation.
These are consumer lifetime obligations, not properties a concept can prove.
There is no `std::function` storage or copyability requirement. Prefer named
coroutine helpers with explicit parameters; invoking a temporary capturing
coroutine lambda does not transfer its closure into its coroutine frame.

## Motivation

[RFC 0002](../cur/rfc-0002-firm-frame-arenas.md) separates frame provision from
task execution. Recipes defer frame allocation until the receiving context is
established and admission has been reserved. Historically this motivated the
following proposed spelling (not an implemented API):

```cpp
firm::of(f, g)
```

over preconstructed tasks:

```cpp
auto a = f();
auto b = g();
firm::of(std::move(a), std::move(b));
```

The latter has already made the frame allocation decision. A recipe lets an
owner invoke it in the right place. That owner need not be a firm: the pool
owns direct tasks, while still using the ambient firm's frame provider.

## Algebra Direction

Once work is represented as ideas, value composition becomes natural:

```text
f & g     both, like when_all
f | g     race or choose, like wait_any
f >> g    sequence
f + g     collect or combine, exact meaning open
```

Operator overloading may or may not be the final API. The useful idea is an
algebra of task recipes that lowers into firms, deeds, joins, cancellation, and
timeouts.

For example:

```cpp
auto both = all(read_head, read_body);
auto first = race(timeout, response);
auto flow = connect >> handshake >> request;
```

Names such as `all`, `race`, and `then` may be clearer than symbolic operators
for the first implementation. Operators can be added only where the meaning is
pleasant and unsurprising.

## Implemented: task tuples

The first concrete API uses the existing names and an explicit tuple, rather
than new operators or a type-erased `idea` wrapper:

```cpp
auto [config, index] = co_await when_all(std::tuple{
    [&] { return read_config(); },
    [&] { return read_index(); },
});

auto answer = co_await wait_any(std::tuple{
    [&] { return query_primary(); },
    [&] { return query_replica(); },
});

auto outcomes = co_await with_firm<stop_on_completion>(std::tuple{
    [&] { return response_task(); },
    [&] { return timeout_after(1s); },
});
```

Each tuple element can be a task or an owned nullary task factory. Factories
are invoked once from finite indexed recipes in the existing pool. Their
storage stays alive through settlement, including cancellation or an exception
while invoking a later factory. This also keeps a capturing coroutine
factory's closure alive; ordinary `fork(temporary_coroutine_lambda)` does
**not** gain that guarantee. Preconstructed tasks retain their original frame
allocation.

The lowering produces `task<void>` pool jobs; each writes its typed
`expected<T, exception_ptr>` outcome to its matching tuple position. The main
work creates no firm child records or deeds. Policies receive
`firm::completed(task_id, exception_ptr)` notifications for pool-owned work, sharing
the completion policy hook used by dynamic firm children.
`with_firm<Policy>(tuple)` therefore returns a tuple of settled expected
outcomes, not `catching_deed<T>` handles. The policy controls sibling
cancellation, and the caller selects outcomes. This is a return-type change:
read each expected directly instead of calling a deed's `.get()`. Custom firm
policies now override `completed(task_id, exception_ptr)` rather than depending
on the nursery-specific child-record callback.
`when_all(tuple)` stops siblings on failure and returns values in tuple order,
using `std::monostate` for void positions. The empty tuple succeeds.
`wait_any(tuple)` requires a nonempty tuple of matching result types (including
void), stops after success, drains the batch, then returns the first successful
result in tuple order (not completion order); it groups failures if none
succeeds. Failure alone does not win this race. `stop_on_completion` instead
stops siblings on either success or failure; `with_timeout` and
`poll_until_after` use this tuple path. Variadic forms delegate to tuples.

The finite indexed pool batch does not make the firm an exact-size nursery or
promise exactly N records, a fixed frame-byte budget, or allocation-free
execution. The firm still supplies frames and stop policy, and its growable
nursery accepts explicit nested ambient forks; those forks remain outside the
fixed pool bound. This is a real unification of ownership and drain machinery,
not a strict transitive static team.

Admission remains open: a child can fork into its ambient firm without opening
a nested firm. Readiness/deadline races and the cgroup sampling batch use the
tuple syntax for composition, not as a transitive capacity boundary. The tuple
helpers currently accept task values and task factories; the broader `idea`
concept does not by itself add hope-producing factories to those helpers.

Settlement records retain their bidirectional deed link after evacuating a task
frame. Moving a joined deed retargets the record; destroying either side detaches
the other. Clearing only the deed's link at evacuation left a stale pointer in
bookkeeping, exposed when returning tuple outcomes. Records also
retain observation when a deed is released, so a later join does not report an
already handled failure again.

## Implemented: bounded pools

A [pool](../../docs/rt-pool.md) turns `feed<Idea>` into a completion-order
`feed<pool_result_t<Idea>>` (`T` for ordinary value results, `std::monostate`
for void). It borrows a fixed farm of stable slots and output storage, and
reserves admission before reading and invoking another recipe. The accounting
is:

```text
free + reserved for input + running + completed/unconsumed = capacity
```

Here “running” includes admitted pending tasks not yet scheduled, not only
tasks currently executing.

Consumption returns credit; merely finishing a job does not. A completed
task's frame may be destroyed after its result is moved into output storage,
before the consumer returns the slot. Recipes remain alive through settlement
and output consumption. A ready hope needs no coroutine frame.

Pool jobs are directly owned tasks, not firm children or permanent workers.
The pool still uses the ambient firm's frame provider, which must outlive
drain. Known slot and result land is not a frame-byte, response-body, or global
memory bound. `farm::try_alloc()` does not wait: the pool provides the waiting
discipline and deck-local completion wakeups.

## Future: teams, outcomes, and terminal consumers

The proposed distinction is a heterogeneous static **team** versus a
homogeneous **pool** with circulating capacity. Fixed tuple helpers now lower
to finite indexed jobs in the existing pool and return typed settled outcomes;
they do not allocate main-work child records. They are not strict transitive
static teams: nested explicit ambient forks remain in the firm's growable
nursery and are outside the tuple batch bound. Nor is this an allocation-free
or frame-budget contract. The `idea` concept is shared recipe vocabulary, not
an owning wrapper or an admission policy.

An unhandled pool job failure stops admission and causes cancellation/drain
of the pending upstream read and admitted jobs when the consumer encounters it.
That is not generic closure of a borrowed upstream producer.
For the general stream pool, final-suspend notification does not itself apply
a fail-fast policy. Fixed tuple jobs instead report through the shared
`firm::completed(task_id, exception_ptr)` hook; their typed expected outcomes
are stored separately from that notification. Planned idea-level `cope` would
turn acceptable per-job failures into expected outcome values before the pool.
That preserves the distinction between a successful outcome describing a
failed attempt and a failure of the stream itself. Neither that adaptor nor
generic feed `map` is implemented yet.

Stream first-success, collect, and drain should be lifetime-aware terminal feed
operations: they must close and drain upstream work before releasing borrowed
state, including on early success or cancellation. They are not a reason for
raw borrowed `take()` to auto-close a source. Today explicit cleanup is
required, as shown in the pool guide; the terminal combinator API remains open.

## Time As Territory

The runtime is moving toward explicit space budgets through frame arenas,
feeds, and pool slots, with I/O buffer groups still proposed. These are
separate capacities, not a claim that all task bookkeeping or memory is
bounded. Time should eventually receive similarly explicit treatment.

There should be no implicit unbounded suspension in high-level composition. An
idea algebra should make waiting policies visible:

```cpp
with_timeout(idea, 200ms)
with_budget(idea, time_slice)
race(work, timeout_after(1s))
```

This does not mean every low-level await carries a deadline immediately. It
does mean the composition layer should have a place for time budgets from the
start.

## Relationship To Firms

One proposed lowering for fixed compositions uses firms:

- `all(f, g)` creates a firm, forks both ideas, joins both, and combines deeds.
- `race(f, g)` creates a firm, forks both ideas, stops siblings when one wins,
  and joins the rest.
- `then(f, g)` awaits the first result, then invokes the second idea in a
  firm-visible context.

This is not the only lowering: homogeneous streams already use the pool's
direct task ownership. Structured lifetime does not require adding firm child
records to every owner.

Fork failure is a synchronous allocation/bookkeeping error and should initially
throw with a structured diagnostic.

## Relationship To Wishes

Wishes remain analyzable values:

```cpp
op::recv_some{fd, max}
```

Ideas are opaque callable recipes:

```cpp
auto receive_loop = [&] { return run_receive_loop(socket); };
```

Here `run_receive_loop` is a named coroutine helper; the closure itself is
not a coroutine.

The algebra may lift wishes into ideas:

```cpp
idea_from(op::timeout::after(1s))
```

but it should not pretend that every idea can be inspected like a wish.

## Open Questions

- Which operators are genuinely readable enough to keep?
- How do time budgets compose through `all`, `race`, and `then`?
- Should future static teams use named composition helpers or a distinct owner
  API, alongside the implemented tuple-to-pool helpers?
- How should failures be reported for composed ideas: first error,
  `exception_group`, or policy-specific result?

## References

- [RFC 0002: Firm Frame Arenas](../cur/rfc-0002-firm-frame-arenas.md)
- [RFC 0005: Firm Bookkeeping without Heap Vectors](../cur/rfc-0005-firm-bookkeeping-without-heap-vectors.md)
- [RFC 0006: Join as a Completion Feed](rfc-0006-join-as-a-completion-feed.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [task.hpp](../../src/nxtrt/task.hpp)
- [exceptions.hpp](../../src/nxtrt/exceptions.hpp)
