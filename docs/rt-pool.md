# Bounded idea pools {#rt_pool}

A `pool<Idea>` turns a `feed<Idea>` into a `feed<pool_result_t<Idea>>`.
It admits work into borrowed slots and publishes results in completion order.
It is a stream stage, not a body lambda, an unrestricted nursery, or a set of
permanent workers.

An `idea` is a concept: a concrete, move-constructible callable returning a
`task<T>` or `hope<T>`. The pool invokes each admitted recipe once, as a stored
lvalue. It preserves that recipe until the task has settled and its output has
been consumed. Pending tasks are owned directly by slots, without firm child
records or deeds. Ready hopes can pass through without creating any coroutine.
Void ideas produce `std::monostate` items.

## Land and admission

The caller lends three pieces:

- a feed of recipes;
- a fully free `farm<pool_slot<Idea>>`, exclusively for this pool's use;
- raw output storage with at least one cell per slot.

The farm already separates typed slot land from its free-index bookkeeping.
`try_alloc()` is its synchronous admission primitive; the pool supplies the
waiting and completion discipline. Slot and result land do not grow. A slot is
reserved **before** reading another idea, and before invoking it.
The output ring uses exactly the slot count even if the supplied region is
larger: lookahead cannot demand more results than admission can hold.

The accounting rule is:

```
free + reserved for input + running + completed/unconsumed = capacity
```

Completed jobs continue to occupy admission capacity. Their results move into
the output ring, and their task frames can then be destroyed. The slot returns
to the farm only when the output leaves this source, or close discards it.
Completion and consumption are different events.

The output follows ordinary feed semantics. `peek` retains credit; taking,
discarding, or streaming values returns credit as the source consumes them.
This does not mean every downstream layer has finished with a transferred
value. A pending sink write may own a moved value after the source has consumed
it. Returning credit never eagerly reads upstream or overwrites a borrowed
output view; ordinary feed view-invalidation rules still apply.

This bounds admitted jobs, not every byte in the pipeline. Upstream recipe
buffers, response-body storage, the deck registry, and coroutine frames have
their own budgets. Frames currently use the ambient firm's frame provider,
which must outlive the pool's drain. Recipes may themselves allocate or create
further work; the pool does not claim a transitive bound on that work.

## A small pipeline

```cpp
#include "nxtrt/pool.hpp"
#include <array>
#include <iostream>

struct square_idea {
    int input;

    static nxtrt::task<int> delayed(int value) {
        co_await nxtrt::yield();
        co_return value * value;
    }

    nxtrt::hope<int> operator()() {
        if (input % 2 == 0)
            return nxtrt::hope<int>::ready(input * input);
        return delayed(input);
    }
};

nxtrt::task<void> print_results(nxtrt::feed<int>& results) {
    while (auto value = co_await results.take())
        std::cout << *value << '\n';
}

void example() {
    auto jobs = std::array{
        square_idea{1}, square_idea{2}, square_idea{3}, square_idea{4}};
    auto input_land = nxtrt::static_value_storage<square_idea, 1>{};
    auto input = nxtrt::value_range_source{jobs, input_land.ref()};

    auto slots = std::array<nxtrt::pool_slot<square_idea>, 2>{};
    auto available = nxtrt::farm<nxtrt::pool_slot<square_idea>, 2>{&slots};
    auto output_land = nxtrt::static_value_storage<int, 2>{};
    auto results = nxtrt::pool<square_idea>{input, available, output_land};

    auto deck = nxtrt::deck{};
    deck.sync_wait([&] {
        return nxtrt::finally(
            print_results(results),
            [&] { return results.close(); });
    });
}
```

Both cleanup factories above are ordinary functions returning tasks, not
capturing coroutine lambdas. The pool and all its borrowed land live outside
the consuming coroutine, so cleanup still has valid storage after that
coroutine fails or is cancelled.

Factories must produce fresh execution, not already-started tasks. A recipe
may borrow stable enclosing state, but not a view into an upstream buffer that
is invalidated when the recipe is taken. Results likewise must not retain
unprotected references into a recipe after its slot is returned.

## Progress, completion, and teardown

The pool has one consumer and uses one deck. It starts pending jobs directly,
and wakes its consumer through the deck rather than a backend pipe or timer.
Its final-suspend observers only publish slot readiness and wake the consumer:
they never destroy the frame currently completing.

A suspended upstream read is held separately, using a reserved slot. Completed
jobs can still produce output while that read waits. EOF requires upstream EOF
and all admitted outputs to have been consumed. Temporary lack of input or
completion is not EOF.

Ready work can fill an output batch synchronously. Pending completions are
published in notification order, not slot or input order. The pool stages
results in its own feed ring; existing feed/sink transfer paths then consume
batches without an extra message allocation per result.

When reading encounters an input, invocation, or result-extraction failure, it
stops admission, cancels and drains pending input/jobs, discards remaining
outcomes, and rethrows. Previously consumed outputs are not rolled back.
Failures are not individual output items in this first API. Buffered hot-path
consumption and downstream sink errors follow ordinary feed rules: use
`finally` as above to guarantee close even if those operations throw.

`close()` is idempotent, requests stop, waits for actual settlement, and discards
unconsumed outcomes. It does not require a consumer to make room, and caller
cancellation does not interrupt its drain. Stop is cooperative: a task that
does not settle after cancellation can keep close waiting.

Do not overlap reads or close operations, or call close while a read is
outstanding. Stop may be requested while a read waits; await that reader before
separately closing.
Destruction may discard already-settled state, but destroying a pool with
running tasks or a pending reader aborts rather than leaving dangling borrowed
storage. Reading after close is not supported.

## Relation to firms and future work

Firms remain general structured-concurrency nurseries. A pool is an independent
bounded circulation of homogeneous recipes and results, sharing only the
runtime's task, deck, and frame machinery. It does not add tuple constructors
or a second scheduler.

The slot lifecycle is modeled in `nxtrt/runtime.rkt`, including consumption,
close/discard, and reuse. The model abstracts frame bytes and cancellation
progress; tests exercise the concrete feed and coroutine lifetimes.

Generic feed mapping, feedback channels for crawlers, per-item error values,
and heterogeneous static teams are separate follow-up work. The first pool
provides a concrete place to evaluate those designs without changing firm
semantics.
