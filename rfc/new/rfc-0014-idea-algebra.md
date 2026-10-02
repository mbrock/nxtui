# RFC 0014: Idea Algebra {#rfc_idea_algebra}

Status: new

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

[RFC 0002](../cur/rfc-0002-firm-frame-arenas.md) requires tasks to be born inside a
firm so their frames land in firm territory. That means APIs should prefer
recipes:

```cpp
firm::of(f, g)
```

over preconstructed tasks:

```cpp
auto a = f();
auto b = g();
firm::of(std::move(a), std::move(b));
```

The latter has already made the frame allocation decision. The former lets the
firm create each child in the right place.

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
are invoked once, left to right, under the receiving firm. Their storage stays
alive through settlement, including cancellation or an exception while invoking
a later factory. This also keeps a capturing coroutine factory's closure alive;
ordinary `fork(temporary_coroutine_lambda)` does **not** gain that guarantee.
Preconstructed tasks retain their original frame allocation.

`with_firm<Policy>(tuple)` returns a tuple of settled `catching_deed<T>` values;
the policy controls sibling cancellation, and the caller selects outcomes.
`when_all(tuple)` stops siblings on failure and returns values in tuple order,
using `std::monostate` for void positions. The empty tuple succeeds.
`wait_any(tuple)` requires a nonempty tuple of matching result types (including
void), returns the first successful result selected in tuple order after drain,
and groups failures if none succeeds. Failure alone does not win this race.
`stop_on_completion` instead stops siblings on either success or failure; it is
used by the fixed readiness/deadline pair in `poll_until_after`.

Tuple composition now uses an ordinary growable firm nursery, just like
callable firms and variadic/range combinators. The tuple sizes the initial
batch; it does not promise exactly N bookkeeping slots, a fixed 4 MiB frame
budget, or allocation-free execution. Bookkeeping and frame storage follow
the ordinary nursery implementation rather than a tuple-specific capacity
contract.

Admission remains open: a child can fork into its ambient firm without opening
a nested firm. Readiness/deadline races and the cgroup sampling batch use the
tuple syntax for composition, not as a capacity boundary. The tuple helpers
currently accept task values and task factories; the broader `idea` concept
does not by itself add hope-producing factories to those helpers.

Settlement records retain their bidirectional deed link after evacuating a task
frame. Moving a joined deed retargets the record; destroying either side detaches
the other. Clearing only the deed's link at evacuation left a stale pointer in
bookkeeping, exposed when returning tuple outcomes. Records also
retain observation when a deed is released, so a later join does not report an
already handled failure again.

## Future: teams and pools

A fixed team or a capacity-limited pool can consume ideas and define its own
admission, ownership, and scheduling contract. That is distinct from the
ordinary growable tuple nursery. The `idea` concept is the shared recipe
vocabulary, not an implementation of those owners: it adds neither a queue nor
a frame budget. Any future owner must keep admitted recipe storage stable
through settlement and account for a hope being ready without a task frame.

## Time As Territory

The runtime is already moving toward explicit space budgets: frame arenas,
task tables, feeds, and buffer groups all have visible capacity. Time should
eventually receive the same treatment.

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

The algebra lowers through firms:

- `all(f, g)` creates a firm, forks both ideas, joins both, and combines deeds.
- `race(f, g)` creates a firm, forks both ideas, stops siblings when one wins,
  and joins the rest.
- `then(f, g)` awaits the first result, then invokes the second idea in a
  firm-visible context.

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
- Should `firm::of(f, g)` be the first concrete API before symbolic operators?
- How should failures be reported for composed ideas: first error,
  `exception_group`, or policy-specific result?

## References

- [RFC 0002: Firm Frame Arenas](../cur/rfc-0002-firm-frame-arenas.md)
- [RFC 0005: Firm Bookkeeping without Heap Vectors](../cur/rfc-0005-firm-bookkeeping-without-heap-vectors.md)
- [RFC 0006: Join as a Completion Feed](rfc-0006-join-as-a-completion-feed.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [task.hpp](../../src/nxtrt/task.hpp)
- [exceptions.hpp](../../src/nxtrt/exceptions.hpp)
