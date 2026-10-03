# The nxtrt runtime, as a story about holding work {#rt_holding}

Some thoughts about the domain model under `src/nxtrt`, written as an
exploratory companion to the dry reference page at @ref rt_overview. That
page is a map; this one is the reasoning the map flattens. It builds the
concepts up from intuition, as nouns and verbs, and is honest about where
the code is still a seed.

For the current ownership and completion distinctions, see
[Runtime concurrency direction](rt-concurrency-direction.md); for bounded
streaming work, see the [pool guide](rt-pool.md). The sketches below explain
the holding metaphor, not a replacement for the current API reference.

There is one idea underneath everything here, and it is worth saying before
any of the types: **concurrency is the art of holding work that is not
running right now.** A single thread does one thing, top to bottom. The
moment we want two things, something has to *hold* the thing we are not
currently doing, and hand it back later. Every layer of this runtime is one
more answer to "where is the held work kept, and what decides when it comes
back." As above, so below.

So we will meet, in order, four **holders**: the deck holds ready tasks, the
wand holds parked wishes, the feed holds bytes, and the hope holds a
value-or-the-work-to-get-it. They are the same shape at four sizes. At the
end they want to merge, and the code already knows it.

## A task is work that has not happened yet {#rt_holding_task}

The core value is @ref nxtrt::task "task<T>": a lazy coroutine frame that
uniquely owns its state.

A task does **not** run when you construct it. This is the first and most
important fact. Constructing a task is writing down a wish to compute
something; it is inert until a deck decides to resume it. In that sense a
task is already a held thing the instant it exists — held by whoever owns
the frame, waiting for a scheduler to want it.

When task code awaits another task, the child is enqueued on the active deck
and the parent becomes the child's *continuation*. When the child reaches
final suspend, it does not call the parent inline. It *enqueues* the parent
for a future round. Completion, like everything else here, goes through the
holder. Nothing resumes anything directly; it asks the deck to.

This is deliberate, and it is the reason the rest of the model stays
legible: there is exactly one place where control is handed out, and it is
the deck's ready queue.

## A deck is a buffer with a trivial drain {#rt_holding_deck}

So there is a **deck**. The deck is the cooperative scheduler, and its
entire substance is a queue of resumptions:

```cpp
std::deque<ready_item> ready_;
wand * wand_ = nullptr;
```

A `ready_item` is nothing but a coroutine handle and its promise. To
*schedule* work is to `enqueue` a handle. To *run* work is to resume the
handles that are waiting. That is the whole machine.

Now look at how a round is pumped, because the shape is the point:

```cpp
auto round = std::deque<ready_item>{};
round.swap(ready_);                       // take everything ready NOW
for (auto const & item : round)
    item.resume_if_ready(*this);          // drain it
// tasks made ready DURING the round are left in ready_ for next time
```

`run_ready()` is intentionally **one** round. It swaps out the items that
were ready at the start of the call and drains exactly those. Work that
becomes ready during the round waits for a later round. This is not an
optimization; it is a promise. It keeps scheduling explicit and refuses
surprising reentrancy — in fact the deck throws if you try to pump it from
inside a pump.

Here is the first sighting of the spine. A deck is a **buffer**: the
producer (tasks waking other tasks) writes into `ready_`, the consumer
(`run_ready`) drains it. The "release policy" is trivial — FIFO, one round
at a time, no priorities. Which is to say: *the deck is a buffer whose drain
happens to be dumb, and a scheduler is exactly a buffer whose drain got
clever.* They are the same object seen from two distances. The order in
which tasks land in `ready_` **is** the schedule; even doing nothing fancy,
we have already made a scheduling decision by choosing an enqueue order.

The bridge from ordinary synchronous code into this world is `sync_wait`:
start a root task, then pump until it is done.

```cpp
while (!t.done()) {
    if (ready_.empty()) {
        // suspended, but nothing is left to wake it: deadlock.
        throw runtime_error{...runtime_dump_text()...};
    }
    run_ready();
}
```

That deadlock check is the honest edge of a **seed** runtime. If a task has
suspended and the ready queue is empty, then in a pure-task world nobody can
ever wake it. The thing that is *allowed* to wake it from the outside — a
timer firing, a socket becoming readable — is the next holder.

## A wish is a named desire; a wand is what grants it {#rt_holding_wish}

A pure-task deck can only shuffle work it already owns. To touch the world —
read bytes, wait for a timeout, open a file — task code awaits a **wish**.

A wish is a closed operation value. It names what is wanted and carries
nothing about how to get it:

```cpp
template<typename Result, fixed_string Name>
struct wish { using result_type = Result; ... };
```

"read some bytes," "wait until readable," "sleep until a deadline." The wish
is platform-neutral on purpose. It is a noun, not a verb.

The verb belongs to the **wand**. When a coroutine awaits a wish, the active
deck asks its active wand to `prepare` the operation:

```cpp
template<typename Wish>
urge<typename Wish::result_type>
prepare(deck & d, promise_base & promise, Wish wish)
{
    auto state = std::make_shared<urge_state<result_type>>();
    auto token = prep(d, promise, prepared_wish{wish_variant{wish}, state});
    return urge<result_type>{*this, token, state, ...};
}
```

Preparation stages backend state and hands back a coin — a `coin_t` token
naming this particular pending operation — wrapped in an **urge**. The urge
is the awaitable. Awaiting it parks the coroutine:

```cpp
bool await_ready() const noexcept { return false; }   // always suspend
void await_suspend(coroutine_handle<> awaiting) const; // park as a `need`
T    await_resume() { return state_->take(); }         // result, later
```

A parked coroutine becomes a **need**: a suspended frame stored by coin,
waiting for the platform to say "done."

```cpp
struct need {
    void resume(deck & d) const;     // put the task back on the deck
    std::coroutine_handle<> handle;
    promise_base * promise;
};
```

And here is the second sighting of the spine — almost too on the nose. The
wand's interface is, beneath the magic words, a buffer of outstanding
desires:

```cpp
virtual coin_t prep(deck &, promise_base &, prepared_wish) = 0; // stage
virtual void   suspend(coin_t token, need task)            = 0; // hold
virtual void   cancel(coin_t token)                        = 0; // request stop
virtual void   wave(deck & d)                              = 0; // flush
```

`prep` writes a desire into the wand. `suspend` holds the parked task
against its coin. `wave` is called once after each deck round, and its whole
reason to exist is **batching**: task code gets to stage several operations
synchronously during a round, and only at `wave` does the wand submit them —
as a batch, in whatever order the backend likes.

So the wand is a buffer too. But notice its drain got clever where the
deck's was dumb. It coalesces (`wave` flushes a round's worth of wishes at
once). `cancel` requests cancellation; held backend state remains until the
operation can safely retire. It completes out of order (a coin done by the
kernel wakes its one need, not the others). Same
producer/consumer-with-a-holding-tank as the deck, with a
smarter release decision bolted on. The deck was a buffer with a trivial
scheduler; the wand is a buffer with a real one. And the held thing is just
"work" again — last time resumptions, this time desires.

`wave` is literally a flush. Forget to call it and staged wishes never reach
the platform, exactly the way forgetting to flush a writer means the
buffered bytes never reach the socket. Don't forget to flush.

> **Digression worth marking.** This is why task code stays portable. The
> task names what it wants — the wish; the wand decides how to stage and
> complete it on a particular platform. The concrete wands live at the edge:
> `uring_wand`, `kqueue_wand`. Swap the wand and the same task participates
> in a different I/O world, the way you swap an allocator to change where
> memory lives. The wand is the allocator for *time*.

## Groups and pools: holding a set of jobs {#rt_holding_group}

Before the smallest holder, one structural layer, because it is the same
trick applied to tasks themselves.

A group is a set of ideas awaited by one task. @ref nxtrt::settle "settle"
takes a tuple of tasks or task factories, runs them concurrently, and does not
return until every one of them has settled; the awaiting task is parked the
whole time. The group *holds* its jobs in a @ref nxtrt::pool "pool", one slot
per job, keeping enough shared state to stop them, drain them, and surface each
result or exception in a controlled order.

What the caller gets back is not a handle to running work but a settled
`outcome<T>` per job — `std::expected<T, std::exception_ptr>` — in input
order. Nothing remains to redeem later: by the time the outcomes exist, the
executions are over and their frames are gone. A stop rule decides when a
settled job should stop the others (`stop_on_failure`, `stop_on_success`,
`stop_after_first`, or any `noexcept` predicate over the job's index and
whether it failed). This lets a helper see every outcome before deciding what
to throw, without dropped failures.

`when_all`, `wait_any`, `with_timeout` are not new schedulers. They are
small functions over `settle`, which is written over a pool, which is written
over tasks and the deck. `drain` holds an open-ended feed of ideas the same
way, at most `capacity` at once. The holder nests: a pool is a deck-shaped
idea (hold jobs, release on settlement) scoped to a set of jobs instead of the
whole program. As above, so below, again.

The runtime used to have a separate holder for this, the `firm`, with `fork`,
`join`, and per-child `deed<T>` handles redeemed after the join. It held child
records past settlement and borrowed its scope from a callable body. Once
frames stopped coming from firm arenas, what remained only duplicated task
stop and the pool, and it was removed (RFC 0019).

## A hope is the holder at its smallest {#rt_holding_hope}

Now the leaf, where the whole essay collapses to one type.

Everywhere above, holding had overhead: a coroutine frame, a trip through
the deck, a coin in the wand. But most of the time the work is already done.
The byte you want to read is already in the buffer. Paying for a frame and a
deck round-trip just to discover that is the per-field trampoline this
design exists to kill.

So: a **hope** is the sum of a value already in hand and a pending coroutine
that would produce it.

```cpp
template<typename T>
class hope {
    std::variant<T, task<T>> state_;   // the value, OR the work to get it
};
```

That variant is the thesis of this whole document written as a type. A hope
is `ready(value)` when the answer is already here — awaiting it never
suspends, the value returns inline, no frame is born. It is a `task<T>` when
the answer is not here — awaiting splices that task as the continuation and
pays for a frame only then:

```cpp
bool await_ready() const { return holds_alternative<T>(state_); }
void await_suspend(coroutine_handle<> awaiting) {
    get<task<T>>(state_).splice_onto(awaiting, follow_stop_);
}
```

The source comment calls it "the seam between functional (wish-like) and
coroutine (task-like) composition," and that is exactly right. A reader's
`take(n)` is a plain function returning `hope<T>::ready(span)` on a buffer
hit — no frame, no suspension — and a `task<T>` that loops over real reads
on a miss.

Stand back and the hope is the buffer/scheduler duality compressed to its
irreducible core. `ready(v)` is a buffer with the most trivial drain
imaginable: the data is here, take it. `task<T>` is the full scheduler path:
park, wait, be resumed. **One** awaitable, holding either "already here" or
"the work to get here," choosing per call. Every larger holder in this
runtime — deck, wand, pool — is this same either/or at a bigger grain.

## The feed and the sink: buffers that ARE the buffer {#rt_holding_feed}

The byte-stream layer (`feed` / `bytefeed`, `sink` / `bytesink`) is a
deliberate port of Zig's post-0.15 `std.Io.Reader` / `Writer`, and it is
where the holding idea becomes most physical, because here the held thing is
bytes and the holder **is** the buffer.

The Zig shape is load-bearing: the reader is a struct that *contains* its
buffer, with a small **cold** vtable and a concrete, inline **hot** path.
The hot path (`take`, `peek`) touches the vtable only when the buffer runs
dry; the cold verb (`stream_more`) pushes bytes into a sink, and "refill my
own buffer" is just streaming into myself. The whole design is "make the
buffered case free; pay the vtable only on a refill."

We are stackless where Zig is stackful, so a `co_await` cannot be made
invisible the way a fiber swap can — the async color is viral here. That is
the tax C++ charges. And `hope` is precisely how we make the color *cheap*:
`stream_more()` returns `hope<fare_t>`, not `task<fare_t>`, so a layer that
already holds bytes (decrypted TLS plaintext, an in-memory span) streams
synchronously. A fully-buffered read composes with zero suspensions down a
whole stack of feeds — socket → tls → http_body → sse — because at each
layer the hot path is a buffer hit and the buffer hit is a ready hope. The
hot path stays north of the vtable; the frame is born only on a real refill.

## The merge the code is already shaped toward {#rt_holding_merge}

Here is the payoff, and it is not my invention — it is written in the TODO
list in `buffers.hpp`, under `eager-wand`:

> push the synchronous-completion idea of `stream_more()` down to the wish
> layer — an honest `urge::await_ready()` plus a sync path in
> `wand::prepare` — so a warm `read_some` on the fd also skips the
> round-trip. At that point the buffered feed can BE a wand and `hope`
> dissolves into a single "maybe already here, else suspends" awaitable
> shared by wishes and readers alike.

Read that against everything above. We met four holders that turned out to
be one shape: the deck holds resumptions, the wand holds desires, the feed
holds bytes, the hope holds a value-or-its-work. The only reason they are
four *types* and not one is that today the urge always suspends
(`await_ready` hard-coded to `false`) while the hope gets to say "already
here." Give the urge an honest `await_ready`, give `wand::prepare` a path
that completes a warm operation inline, and the wand stops being
categorically different from the feed. A buffered feed, asked for bytes it
already has, becomes indistinguishable from a wand granting a wish that was
already true. `hope` dissolves because everything becomes a hope.

That is the whole musing made literal in the type system: a scheduler is a
buffer whose drain got clever, a buffer is a scheduler whose release
decision stayed trivial, and the runtime's endgame is the single awaitable —
"maybe already here, else suspend" — that both the wishing layer and the
reading layer share. As above, so below.

And, as always: don't forget to flush.
