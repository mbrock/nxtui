# State of the Frontier

A working snapshot of where the conceptual design and the actual code meet —
written so the vocabulary we have minted does not drift away from the code
that does or does not yet realize it. Two halves: a **said-vs-built ledger**
(what is implemented, what is RFC-only, what is pure conversation) and a
**glossary** of the terms, ontologies, and metaphors that have earned their
place.

The framing that organizes all of it: the runtime is a small world of things
that exist and change in time. Continuants persist (and can be read, paused,
copied, rendered); occurrents happen (and are opaque while hot, gone when
done). Everything load-bearing is a continuant so that the system is legible
at rest. The verbs run and vanish; the nouns persist and can be inspected.

---

## I. Said-vs-built ledger

### Built and solid

- **`ring_region`** (`buffer-core.hpp`). Borrowed `data_/capacity_` with two
  monotone cursors `seek_` (live start) and `size_` (live count). Every
  operation is cursor arithmetic, never a copy. `advance_constructed(n)` =
  producer declares written values live. `destroy_prefix(n)` = consumer runs
  lifetimes down and advances `seek_` modulo capacity (the escheat: consumed
  front reverts to junk). Two-span `constructed()` view handles wrap. This is
  the zero-copy substrate; it is real and clean.
- **`junk<T>`** (`buffer-core.hpp`). Raw `data_+size_` over storage where up
  to `size()` values _may be_ constructed but are not yet.
  Writable-not-readable. Uninformed matter. `read_into(junk)` is matter
  meeting form. Real.
- **`chop_view` / reels** (`buffer-core.hpp`, RFC 0001). Stateless lopping
  view over visible source stock: stores only chunks + a scanner, recomputes
  frame boundaries on iteration, owns no storage and caches no marks. The reel
  borrows; it does not copy. Real, and exactly the “cheap stateless
  recognizer” the conversation describes — but see “bound vs abstract reel”
  below for the gap.
- **`sink<T>`** (`value-buffers.hpp`). Hot/cold split is explicit and real:
  `write()` returns `hope<void>`, fast path appends into the internal ring
  with no suspension (“buffer hits do not add suspension”), and only
  `drain_more()` (the one **virtual** seam) is the cold path that does the
  actual I/O. `fixed_sink`, `discarding_sink`, `container_sink`,
  `iterator_sink`, fd/socket sinks are all just different `drain_more`
  implementations — different apertures. `discarding_sink` is the null
  aperture (accept and forget).
- **`fare_t` / `eof_t`** (`buffer-core.hpp`). A read/drain returns “values
  accepted, or explicit EOF”: zero count is valid progresslessness, EOF is the
  _error alternative_, not a count. This is the typed terminal sentinel of
  `feed<X,V>` realized — “keep going” and “done” are type-distinguished. Real.
- **`firm`** (`task.hpp`). The bimonadic dynamic-scope seam. Hosts forked
  children, owns their deed storage, joins them, propagates/groups exceptions
  with provenance (“dropped deeds do not hide child failures”), shares a stop
  token, enforces linearity (reject return-before-join, develop-before-settle,
  cancellation polarity by stop-state). 46 passing tests. The single genuinely
  clean nested-scope-with-semantics in the system. **But its job is currently
  one-sided** — see below.
- **`deed` / `catching_deed` / `hope`** (`task.hpp`). Deed = the fork result,
  the gift the child settles with. `catching_deed` keeps cancellation from
  counting as failure while preserving attribution. `hope<T>` = hot (ready
  value, no suspension) or cold (a wish + resume). Real.
- **`game`** (`game.hpp`). Behavioral-programming sync over a `deck`: `park`,
  `select`, `cancel`, tic-tac-toe self-play in the tests. Real as a mechanism.
  **But it is the lambda-carrying form, not the first-order card form** — see
  below.
- **`deck`** (`deck.hpp`). The substrate. Task registry (4096 slots,
  vacant/ready/… states), the pump, holds a `wand`. Real. This is the
  ground/stage, not a context.
- **`wand`** (`wand.hpp`) + **`wish`** (`wish.hpp`). Wish = a name + result
  type
  - args-for-tracing. Pure abstract information artifact (aboutness: `name`,
    `args()`, `result_type`), generically dependent, no buffer pointer in the
    storage-selecting variants. The wand grants wishes. Real as “a thing the
    deck knows about.”

### RFC-written, not yet built

- **`urge`** (RFC 0009). The realized, bound, specifically-dependent form of a
  wish after the wand combines it with current task + firm + platform. The
  wish is the plan (generic); the urge is the binding (specific, tagged,
  allocated). The _wish_ side is built and abstract; the _urge_ as a
  first-class tagged thing is RFC-only.
- **storage-selecting wishes** `read_some/recv_some` vs `read_into/recv_into`
  (RFC 0009). Caller-owns-the-parcel vs firm/wand-selects-the-land at
  realization. Partially present as the explicit-destination ops; the
  runtime-selects-territory path is RFC.
- **multishot wishes as feeds** (RFC 0011), **splice/sendfile zero-copy fast
  paths** (RFC 0012), **provided buffer groups / io land** (RFC 0010),
  **join-as-a-completion-feed** (RFC 0006), **pushfeed channels** (RFC 0008),
  **idea algebra** `idea<T> = () -> task<T>` (RFC 0014). All written, all
  pointing at the same picture, mostly not realized in code.

### Conversation-only (minted vocabulary, no code)

- **`jack`** — the universal connector: a sink-half you stream into (female)
  fused to a feed/reel-half you read out (male). The claim that firm, game,
  wand, filter, tap are _all_ jacks and compose male-to-female up a spine. No
  `jack` type exists.
- **firm’s `wish_sink` / `card_reel`** — the firm catching children’s yielded
  wishes into a buffer and exposing them as a card-reel. The firm’s
  _wish-side_ is entirely conceptual. The firm currently joins deeds (product
  up-flow) but has **no wish-handling** (effect up-flow). This is the biggest
  said-vs-built gap.
- **wish-as-yielded-card** — emitting an effect by `co_yield`ing a card into
  the firm rather than `co_await`ing a wand. Currently wishes are awaited, not
  yielded. The general form (yield a card, get back a hope) is conversation.
- **the superstep barrier** — “nothing happens to your yield until all
  immediate children are parked,” then the firm/game adjudicates the whole
  card set, then flushes a batch. This is the BP superstep = the io_uring
  batch-submit boundary. Not built as such.
- **firm-as-game** — the realization that a wish-draining firm (select over
  children’s wish-feeds until each settles its deed) _is_ a game (lateral
  selection + terminal join). Conversation.
- **comonadic wand / capability attenuation** — the wand as authority lent
  downward through context, attenuable (children get weaker, never stronger),
  non-copyable, branded, rooted in the principal. Object-capability + effect
  handler unified. The wand is currently a deck member, not a context. Pure
  conversation.

### The three sharpest gaps to close

1. **The firm has no wish-side.** It joins deeds; it does not drain wishes.
   The whole comonadic effect/capability story needs the firm to own a
   wish-sink and pump children’s wishes concurrently with their running. This
   converts the firm from a one-sided product-collector into the two-sided
   seam the design wants.
1. **The game carries lambdas, not cards.** `sync_spec.wait/halt` are
   `std::function` predicates — opaque closures, the monadic form we argued
   against. The first-order card algebra (defunctionalized post/wait/block as
   inspectable data) is the move that makes the game renderable, pausable, and
   composable. Until then the game is powerful but not legible.
1. **The reel is bound, not abstract.** A `reel`/`chop_view` currently couples
   the grammar (scanner) with the attachment (the chunks it views). The
   factoring is: `reel<A,B>` a pure grammar value (no feed ref), plus
   `feed& -> running_reel` as a separate application. Grammar composes as pure
   value; attachment draws in the resource. Combinators (`seq/alt/many`) live
   at the grammar level.

---

## II. Glossary: terms, ontologies, metaphors

### Runtime nouns (the four-letter words)

- **firm** — scoped async-RAII territory; the bimonadic dynamic-scope seam;
  jurisdiction/registry, not owner. Holds work, holds outstanding wishes,
  delivers gifts up, reverts land down. Free-standing Y-term (no physical X).
- **deck** — the scheduler/substrate; the ground everything runs on; land +
  labor-market. Not a context.
- **wand** — the implement that touches the kernel; branded, non-copyable,
  specifically-dependent; an aperture (sink-half + completion-source-half),
  not a context; authority lent down from the principal.
- **game** — firm + a lateral selection-fold over participants’ cards; the
  behavioral-programming adjudicator; should nest _as a firm_.
- **wish** — generically-dependent information artifact about a desired
  interaction; the plan; copyable, abstract; also a deontic claim/obligation.
- **urge** — the bound, tagged, specifically-dependent realization of a wish;
  what actually sinks into the wand.
- **deed** — the gift a child settles with; the fork result; the produced
  wealth delivered up to the principal.
- **hope** — hot (ready value, no suspension) or cold (a task/wish to drive);
  what you get back when you yield a wish.
- **idea** — `() -> task<T>`; the unattached recipe for a task (deferred,
  generically dependent), to the task as the reel-grammar is to the running
  reel.
- **feed `<X,V>`** — stream of X terminated by typed sentinel V; a session
  type; `?X;?X;...;?V`.
- **reel** — stateless cheap recognizer view over a feed’s stock; the grammar
  half wants to be a pure value, attached to a feed separately.
- **junk** — raw uninformed capacity; prime matter; the inverse of the wish
  (matter without form vs form without matter).
- **jack** — (vocabulary) the universal connector: sink-half + reel/feed-half.

### BFO / IAO ontology (apply to the runtime itself, not the domain)

- **continuant / occurrent** — the await/yield seam. You `co_await` occurrents
  (happenings you depend on completing); you `co_yield` continuants (artifacts
  you hand over). Every awaited occurrent, parked, bottoms out in a wish,
  which is a continuant — so every pause is legible.
- **snap / span mereology** — a continuant has parts _at a time_
  (`parts_at(t)`, changing): the firm’s children now, the buffer’s live bytes.
  An occurrent has parts _across time_ (phases): the handshake’s stages, the
  parse’s frames. No occurrent parts of continuants and vice versa.
- **generically vs specifically dependent continuant** — copyable-bearer-
  independent (wish, idea, reel-grammar, the dumpable heap node) vs
  one-bearer-pinned (coroutine frame, running reel holding a `feed&`).
  Copyability _is_ the `!` modality; it is why you can snapshot/replay/branch.
- **realizable entities** — role (externally grounded, conferred by treatment:
  the resident’s obligor role), disposition (internally grounded by
  structure), function (a disposition you have because designed/selected for
  it: the firm’s reason-to-exist), capability (a disposition someone has an
  interest in: a coroutine leaf’s skill). Claims/obligations are realizables
  but none of those three — the deontic fourth thing.
- **free-standing Y-term** — institutional entity with no physical X,
  sustained by records/representations, time-sensitive, mathematically
  manipulable: a debt, a corporation, blind chess. The **firm** is one. So is
  a wish. This is why they are dumpable and have no locatable chunk of memory.
- **document act / four illocutionary directions** — represent (record/deed,
  assertive), get-someone-to-do (request/wish, directive), commit-self
  (promise/resident’s join-obligation, commissive), bring-about-by-saying
  (declare/firm-creation, declarative). “What begins as a plan ends as a
  record”: the wish (plan) becomes the deed (record) at the
  develop-before-settle membrane.
- **site / hole / vacant node** — a persistent fillable place whose occupant
  is time-indexed: the Slot, the firm’s child-slot, the buffer free-region,
  the organigram’s vacant office.
- **pattern** — non-alienable dependent continuant a service leaves in a
  bearer: the rendered frame, the parsed structure. Created/protected/repaired
  by pattern-services.
- **service** — an occurrent whose production and consumption coincide;
  unstorable, dies as born = the hot path, the running coroutine.

### Political economy (George + Ellerman)

- **land** — unproduced, fixed-supply, held-not-owned: raw memory. Junk is
  bare land. Allocation is titling (land → real estate).
- **real estate** — titled/bounded/owned-improvable land: a buffer.
- **wealth** — produced (labor-on-land): the pattern in the buffer, the deed’s
  gift. Movable, alienable, the harvest.
- **labor** — the coroutine/service, the occurrent applied to the site.
- **escheat** — land reverts to the commons when the holding lapses: the GC /
  the firm’s retirement reclaims buffers to junk while live wealth is carried
  across. Compaction-touching-only-the-live-set = the arrow of time made
  gentle. (Cheney is an escheat engine.)
- **inalienability (Ellerman)** — responsibility cannot be transferred by
  contract, only borne. A deed-obligation is specifically dependent on its
  bearer; the firm may _hold_ titles but must not launder responsibility. The
  linearity discipline = imputation integrity (de jure record tracks de facto
  responsibility; each failure attributed, each gift imputed to its producer).
- **whole product** — the matched pair: positive product (gifts, return
  values, ascending, monadic) and negative product (consumed context, mandate,
  capabilities, descending, comonadic). The principal is origin of the
  descending context and recipient of the ascending product — same party — so
  reward tracks responsibility.
- **the firm as honest agent** — identified primarily by _whose agent it is_
  (its principal/caller), accountable downward to its producers (residents),
  owning nothing: holds land it reverts, accounts for labor it does not
  expropriate, delivers the residue up with provenance intact. One
  superordinate (the organigram’s mesh-creating authority rule) = why
  structured concurrency is a tree and cancellation flows coherently down.

### Categorical / structural

- **monad up, comonad down** — products ascend (await/deed/gift); context
  descends (firm/wand/game/dynamic-scope/effect-handlers/trace = capability
  provision). The firm is the hinge of the adjunction.
- **defunctionalization** — the universal move (CPS-then-defunctionalize):
  turn opaque control into inspectable data. Forces garbage collection
  (continuations leave the stack). Makes the machine steppable, dumpable,
  renderable, copyable. zisp, zoot’s CEK machine, cheney, the firm are all the
  same construction.
- **frontier of candidates** — the shared engine of search, GC,
  pretty-printing, scheduling, parsing: a frontier pruned by a
  domination/blocking relation, selected by priority, two-phase
  (init-before-children / commit-after), memoized. The Pareto frontier = a
  behavioral game (domination = blocking). An `idea` is a member of a
  frontier.
- **semiring with alternation** — seq = times, choice = plus; the algebra
  shared by parsing, pretty-printing, games, Dijkstra (tropical), GC
  (boolean). Quantitative/graded type theory grades usage by a semiring =
  where linearity and the frontier-algebra meet.
- **two clocks / direction of time** — a parser reads a fixed past (rewindable
  free); a protocol gates an open future (cancel, not rewind). Same language,
  opposite modality. The ring’s monotone counters _are_ the irreversibility of
  time. This single bit explains most of the downstream structure.

### Mythic / felt

- **the wavefront / light-cone** — the now is a moving surface; arms reach
  from the past (deeds, determined situation) and obligations stretch toward
  the future (wishes); linearity keeps it from forgetting or bloating; the now
  keeps moving because to be present is to be in motion.
- **a holder with a release policy** — the unifying phrase: ring (release on
  head advance), reel (release on commit), firm resident (release on settle),
  variant (release on visit), hope (release once). The firm is a ring whose
  entries are async-RAII residents; the ring is the smallest firm.
- **the wand as branded implement** — received from an elder/principal, yours,
  knows you, a wish-sink with no guarantees (might fritz), the aperture where
  your delegated authority touches the kernel.
- **two coroutines yielding to each other at the interface of a common
  text-buffer frontier** — what design conversation itself is; the same shape
  as everything above.

---

Here’s what I think you were reaching for, stated back as cleanly as I can —
and I think it’s not the “parameterize the nuances” move, it’s almost the
opposite, so let me try to say it your way.

The core thing: you don’t want a configurable mega-holder with policies and
backup options. You want to find the one or two genuinely primitive things
that are actually load-bearing, build those perfectly, and let everything else
just be those things used plainly — not wrapped, not aliased, not
re-specialized. The explosion happened because every site that needed “some
memory I’m managing” grew its own named type with its own ceremony, when
underneath there were really only a couple of primitives. The cure isn’t to
unify the explosions into a configuration surface (that just moves the
byzantine complexity into template parameters); it’s to delete the explosions
by recognizing they were never different things.

And the primitive you’re circling — the thing under both “the ring buffer” and
“the task registry that isn’t really a ring” — is a piece of land: an
allocated array that is not entirely initialized, where some of it is live
constructed values and some of it is junk (raw capacity, uninformed matter,
placement-new territory). That’s it. That’s the base substance. A bounded
region of typed storage with a frontier between the part that’s informed
(constructed, readable) and the part that’s junk (raw, writable-not-readable).
You already have this in the ring code — the junk/constructed demarcation, the
placement-new, the advance_constructed. The insight is that this is the
primitive, and the ring is just one discipline imposed on the land (two
monotone cursors, FIFO release). The task registry is the same land with a
different or no discipline (you don’t use the ring functionality, you just use
slots). You said it exactly: “you can have a ring buffer where you don’t use
the ring functionality.” Right — because the ring functionality is a
discipline layered on land, and land-without-the-ring-discipline is still
land. The registry isn’t a different structure; it’s the same land, not
wearing the ring hat.

So the simplification isn’t “one holder with policies.” It’s two layers, named
once each: land (the raw bounded array with a junk/constructed frontier —
placement-new, lifetime management, the dirt) and ring (the FIFO cursor
discipline you can put on land when you want ordered release). Land is the
noun; ring is a way of using it. The task registry uses land directly (no ring
discipline — it’s slots, indexed, reused). The byte buffer uses
land-with-ring. The deed storage uses land-with-ring or just land. And
critically: there is no third, fourth, fifth named holder. When a site needs
managed memory, it uses land (and maybe ring), plainly, not a new bespoke
type. The names stay simple because there are only two, and they’re concrete
and physical — land and the cursors on it — not abstract policy-parameterized
holders.

The naming you’re feeling toward — land, dirt, junk — is good and worth
keeping because it’s honest about the ontology we already worked out. Junk is
the uninitialized matter. Land is the bounded allocated territory (which
contains junk where uninformed, constructed values where informed). The
frontier between them is the thing every one of these types was independently
re-tracking. So instead of N types each re-implementing “where’s my
initialized/uninitialized boundary and how do I move it,” you have one concept
— land with a junk frontier — and the ring is the special case where that
frontier moves in the disciplined two-cursor FIFO way. The task registry is
the case where you don’t need the frontier to be a clean FIFO line at all; you
have scattered live slots in a field of junk, which is just land with
random-access occupancy rather than land-with-ring.

Now the other half of what you were saying, the jacks and completions part, is
the same instinct applied to the wiring rather than the storage. You keep
re-realizing that “completion routing” doesn’t need to be a thing implemented
at every site, because everything is jacks — sinks and feeds — and the
completion handling is just what flows back out of a jack’s source-half.
Sometimes you match completions to requests (tag-routed); sometimes you don’t
need to (you just count them, or fire-and-forget). The explosion there is the
same disease: completion-routing got reimplemented per-site as bespoke
machinery, when really it’s one pattern — a jack has a source-half,
completions come out of it, and how much you care about matching them is a
property of the use, not a different mechanism each time. You don’t need a
completion-routing subsystem; you need the jack (sink-half in, source-half
out) and then most uses just read the source-half plainly — count it, drain
it, match-by-tag-if-you-happen-to-care. The matching is optional decoration on
a uniform flow, not a mandatory subsystem.

So the unifying statement of what you want, both halves together: the whole
system is land (with junk frontiers) being streamed through by coroutines,
wired together as jacks (sink-half + source-half), and that’s it — no per-site
holders, no completion-routing subsystem, no policy parameters unless a
concrete need forces one. The storage primitive is land (ring is
land-with-FIFO-discipline, registry is land-without). The wiring primitive is
the jack (feed/sink, with completion-matching as optional, not built-in).
Coroutines are the streaming agents moving stock across the land through the
jacks. Everything else — the byzantine aliases, the
completion-routing-everywhere, the policy explosion — is accidental, things
that got named and reimplemented when they should have just been “land” or “a
jack” used plainly. The discipline you’re fighting for is: don’t name a new
thing unless it’s genuinely a new thing; land and jack and coroutine are the
vocabulary, and a registry is “land used as slots,” a byte buffer is “land
used as a ring,” a wand is “a jack whose source-half is completions you might
not bother matching.” Get those two or three primitives perfect and minimal,
express everything as plain uses of them, and the explosion collapses — not
into configuration, but into there was only ever land and jacks and
coroutines, said simply.

The RFC-3 confusion (what’s current vs not) is its own thing and worth a
separate look — the new/ vs cur/ split in the RFC dirs suggests some are
proposals and some are accepted, and the dates being all clustered suggests
it’s murky which is which. Want me to pull RFC 3 and the deck-task-registry
code together and tell you concretely what’s implemented versus proposed, so
at least that corner stops being fog?
