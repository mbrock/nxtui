# RFC 0014: Idea Algebra {#rfc_idea_algebra}

Status: new

## Summary

Task composition should mostly operate on task factories, not on already-born
`task<T>` values.

This RFC introduces the working word `idea<T>` for a callable that can produce
a fresh `task<T>` inside the current firm:

```cpp
template<typename F, typename T>
concept idea_of =
    std::invocable<F &>
    && std::same_as<std::invoke_result_t<F &>, task<T>>;
```

The exact spelling may be a concept, alias family, or wrapper type. The
important distinction is:

```text
wish      = analyzable value recipe for outside work
idea<T>   = opaque callable recipe for a task<T>
task<T>   = already-born coroutine frame
```

An idea is a degenerate wish in one sense: it is a desire to compute `T`. But
unlike a wish, it is opaque. The runtime cannot inspect it until it is invoked
and the task frame is born.

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

## Implemented: closed task tuples

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

For N tuple elements, child, deed, completion and join-failure bookkeeping each
have exactly N inline slots, rather than four default allocations sized for
4,096 children. The backing lives in the owning coroutine frame; the movable
policy borrows it. The existing 4 MiB frame arena remains separately allocated:
task count does not bound the bytes used by nested coroutine calls, so this is
not an allocation-free API.

The tuple explicitly closes admission. A child that wants more forks must open
a nested firm. Existing callable firms, variadic/range combinators, and
`with_timeout` keep their open admission behavior: arbitrary tasks, especially
HTTP handlers, may already fork into their ambient firm. Only known closed
sets have migrated: readiness/deadline races and the cgroup sampling batch.

Settlement records retain their bidirectional deed link after evacuating a task
frame. Moving a joined deed retargets the record; destroying either side detaches
the other. Clearing only the deed's link at evacuation left a stale pointer in
borrowed bookkeeping, exposed when returning fixed-set outcomes. Records also
retain observation when a deed is released, so a later join does not report an
already handled failure again.

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

Ideas are opaque callable task recipes:

```cpp
auto receive_loop = [&] -> task<void> { ... };
```

The algebra may lift wishes into ideas:

```cpp
idea_from(op::timeout::after(1s))
```

but it should not pretend that every idea can be inspected like a wish.

## Open Questions

- Is `idea<T>` only a concept, or should there be an owning type-erased wrapper?
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
