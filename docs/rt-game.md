# The game: behavioral programming on the runtime {#rt_game}

This is a companion to @ref rt_overview and @ref rt_holding. It is about
@ref nxtrt::game "game<Event>", the one piece of the runtime that is not
plumbing — it is a *programming model*. The other holders move bytes and
resumptions around; the game coordinates **behavior**.

The idea is **behavioral programming** (BP), as introduced by Harel, Marron,
and Weiss: instead of writing one tangled state machine, you write many
small, independent **b-threads**, each describing one slice of a
requirement, and a coordinator weaves their declarations into a single
consistent run. The lineage here is concrete. The direct ancestor is the
tiny BP engine in `~/repos/swash-2024/src/sync.ts`, which hosted BP on top
of effection's generators; its `Sync` record had exactly the fields ours
does — `post`, `wait`, `halt`. `nxtrt::game` ports that engine onto the
runtime's own coroutines: a b-thread is a @ref nxtrt::task "task", a sync
point is a `co_await`, and the b-threads are structured children of a
@ref nxtrt::firm "firm".

## The game is a holder, too {#rt_game_holder}

@ref rt_holding made a claim: every layer of this runtime holds work that is
not running, and differs only in what decides when the work comes back. The
deck holds resumptions and releases them FIFO. The wand holds desires and
releases them in platform-batched waves. The game is the third member of
that family, and it has the most interesting release rule of all.

The game holds **parked b-threads**:

```cpp
std::vector<participant> participants_;   // every b-thread waiting at a sync
```

and it releases them by **negotiation**. Where the deck asks "who is next?"
and the wand asks "what does the platform say is ready?", the game asks
"given everyone's declared wishes and vetoes, *what is allowed to happen
next* — and who cares about it?" Same holder shape; a release discipline
made of consensus instead of order or readiness. That is the whole idea, and
the rest is mechanism.

## Three declarations at every sync point {#rt_game_spec}

A b-thread does not call anyone. At each decision point it pauses and
*declares* its stance toward the events that might happen, as a
@ref nxtrt::sync_spec "sync_spec":

```cpp
template<typename Event>
struct sync_spec {
    std::vector<Event> post;                       // request: I propose these
    predicate          wait = [](Event const&){...}; // waitFor: wake me on these
    predicate          halt = [](Event const&){...}; // block: I forbid these
};
```

These are BP's three idioms, under nxtrt names:

- **post** — *request.* "I would like one of these events to be chosen." A
  b-thread that only wants to drive things forward posts and waits for its
  own posts.
- **wait** — *waitFor.* "I am not asking for anything, but if an event
  matching this predicate is chosen, wake me — I have an opinion about what
  happens next."
- **halt** — *block.* "While I sit at this sync point, this event may **not**
  be chosen, no matter who requested it." This is the veto, and it is where
  the power lives.

A sync point is reached by awaiting the game's sync, with sugar for the
common cases via `co_yield`:

```cpp
co_await game.sync({ .post = {a, b}, .halt = forbids });  // explicit
co_yield sync_spec<Event>{ .wait = wants_move };          // a full spec
co_yield some_event;                                      // == request one event
```

The `co_yield` forms work because @ref nxtrt::detail::promise_base
"promise_base::yield_value" routes straight into `game_yield`, so any task
running inside a game can speak BP without naming the game at all. Between
two syncs, a b-thread is exactly a held thing — parked in `participants_`,
its coroutine frame suspended, waiting for a turn to select an event it
cares about.

## The super-step: one turn of the coordinator {#rt_game_turn}

BP runs in **super-steps**. All b-threads declare; the coordinator picks one
event consistent with every declaration; the affected b-threads advance and
re-declare; repeat. The game realizes a super-step as a single coordinator
task, scheduled once per batch of parks:

```cpp
void schedule(deck & d) {
    if (coordinator_active_) return;     // one coordinator per turn, not per park
    coordinator_active_ = true;
    coordinator_ = coordinator_task();
    d.start(coordinator_);
}

task<void> coordinator_task() {
    co_await yield();                    // let the whole round's parks settle
    coordinator_active_ = false;
    drain();                             // then run exactly one turn
}
```

The `co_await yield()` is load-bearing: it lets every b-thread that syncs
during the current deck round land in `participants_` *before* any selection
happens, so the turn sees all of their declarations at once. That is what
makes it a super-step rather than a race.

Selection is the BP rule, stated plainly — the first **requested** event
that **no parked b-thread blocks**:

```cpp
std::optional<Event> select_event() const {
    for (auto const & p : participants_)             // priority = park order
        for (auto const & event : p.awaiter->spec_.post)
            if (!blocked(event))                     // no halt() forbids it
                return event;
    return {};                                       // stalemate
}
```

Then publication wakes everyone who **requested or waited for** the chosen
event; the rest stay parked and will be reconsidered next turn:

```cpp
if (requested(spec, event) || spec.wait(event)) {
    participant.awaiter->selected_ = event;          // this b-thread advances
    ready.push_back(participant);
} else {
    remaining.push_back(participant);                // still parked
}
```

Each woken b-thread resumes from its `co_await`, runs until its next sync,
and parks again — which schedules the next coordinator turn. One event per
super-step, iterated until the run reaches a stalemate or the firm stops.

A note on priority: swash sorted b-threads by an explicit `prio` field. The
game instead uses **park order** — `participants_` is filled in fork order,
and `select_event` scans it front to back, so earlier-forked b-threads (and
earlier entries in a single `post` list) win ties. Priority is positional,
not numeric. The tic-tac-toe AI below leans on exactly this.

## Why `block` is the whole point: tic-tac-toe {#rt_game_ttt}

The canonical BP demonstration is tic-tac-toe, because it shows the property
that makes BP worth having: **requirements compose by addition.** Each rule
of the game is its own b-thread, and adding a rule never means editing an
existing one. The runtime's own test (`test/runtime-test.cpp`) builds the
whole game this way. A few of the b-threads, verbatim:

**Turn alternation** — enforced purely by blocking the wrong player. This
b-thread requests nothing; it only forbids:

```cpp
task<void> ttt_enforce_turns() {
    for (;;) {
        co_yield sync_spec<ttt_event>{                    // X's turn:
            .wait = [](auto& e){ return e.is_move() && e.player == 'X'; },
            .halt = [](auto& e){ return e.is_move() && e.player == 'O'; }, // O blocked
        };
        co_yield sync_spec<ttt_event>{                    // then O's turn:
            .wait = [](auto& e){ return e.is_move() && e.player == 'O'; },
            .halt = [](auto& e){ return e.is_move() && e.player == 'X'; }, // X blocked
        };
    }
}
```

**No playing an occupied square** — one b-thread *per cell*, each of which
waits to see its square taken, then blocks that square forever after:

```cpp
task<void> ttt_square_taken(int row, int col) {
    auto is_square = [=](auto& e){ return e.is_move() && e.row == row && e.col == col; };
    co_yield sync_spec<ttt_event>{ .wait = is_square };        // wait until taken
    for (;;)
        co_yield sync_spec<ttt_event>{ .halt = is_square };    // then forbid replays
}
```

**A dumb strategy** is just a list of requests — `ttt_x_script` posts its
moves in order and lets the blockers above keep it legal. **A smarter
strategy** (`ttt_o_ai`) posts a whole *preference list* in one `post`, and
positional priority plus the blockers pick the best still-legal move:

```cpp
co_yield sync_spec<ttt_event>{
    .post = { ttt_move('O',1,1), ttt_move('O',0,0), ttt_move('O',0,2), ... },
};
```

**An observer** watches every move, maintains a board, and ends the run:

```cpp
if (board.wins(event.player)) {
    co_yield ttt_event{ .kind = win_kind };       // announce the win as an event
    require_current_firm().stop();                // and stop the game
    co_return;
}
```

None of these b-threads knows about the others. `ttt_enforce_turns` has
never heard of squares; `ttt_square_taken` has never heard of turns; the
strategies just request and trust the vetoes. The legal, alternating,
no-replay, terminating game *emerges* from their overlaid declarations. That
is the BP payoff: a new rule is a new b-thread, dropped in beside the
others, blocking what it must — not a surgical edit to a monolith.

## Scoping, ending, and the honest edges {#rt_game_scope}

A game is established for a scope and torn down with it.
@ref nxtrt::with_game "with_game<Event>" installs a fresh `game<Event>` into
the task environment so `current_game<Event>()` can find it, and
@ref nxtrt::sync_wait_game "sync_wait_game<Event>" runs a body under both a
game **and** a firm:

```cpp
sync_wait_game<ttt_event>(deck, [&]() -> task<void> {
    fork(ttt_enforce_turns());
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) fork(ttt_square_taken(r, c));
    fork(ttt_detect_end(board, events));
    fork(ttt_x_script());
    fork(ttt_o_ai());
    co_await join();
});
```

The firm is not incidental. It makes the b-threads structured children: they
can be stopped together, and `require_current_firm().stop()` is how a watcher
ends the whole game cleanly. A stopped b-thread does not vanish silently — it
resumes from its sync with an @ref nxtrt::operation_cancelled
"operation_cancelled", via the `std::stop_callback` the game wired up when it
parked, so cancellation unwinds the same way every other awaited operation in
the runtime does.

Two edges are worth stating honestly, in the spirit of a seed runtime:

- **Stalemate is a deadlock.** If `select_event` finds no requested event
  that is unblocked, the turn selects nothing, nobody resumes, and the
  parked b-threads simply stay parked. Under `sync_wait` that surfaces as the
  deck's deadlock detector firing (see @ref rt_holding). A game is expected
  to end by *requesting* a terminal event and stopping its firm — as
  `ttt_detect_end` does — not by trailing off into a stalemate.

- **No `exec`.** swash's `SyncSpec` carried a fourth field, `exec`: a
  b-thread could attach an asynchronous side effect whose *result became the
  next posted event*, folding I/O into the BP turn. `nxtrt::game` omits this
  deliberately. Effects live in ordinary tasks and wishes (see @ref rt_wand);
  the game stays a pure event-coordination layer, and a b-thread that needs
  to *do* something simply `co_await`s a wish like any other task and posts
  an event when it is done. Re-introducing an `exec`-shaped affordance would
  be a natural extension, but the simpler core has been enough so far.

## Where it sits {#rt_game_close}

So the game completes the holder family. The deck holds resumptions and
drains them in order; the wand holds desires and drains them when the
platform is ready; the game holds b-threads and drains them when the group
*agrees* an event may happen. Three holders, three release disciplines —
sequence, readiness, consensus — over the same coroutine substrate. As
above, so below; and on top of it, a small, composable way to describe what a
program is *allowed* to do.
