# The quest {#quest}

`nxt` is, honestly, a search more than a library. The code runs, but the real
project is the hunt for *one* model of concurrent, effectful computation that
is at the same time **coherent** (the pieces genuinely fit, instead of sitting
in adjacent layers pretending to), **correct** (you can say what it does and
check it), and **efficient** (the already-done case pays for nothing). Most
systems get one or two of those. I want all three at once, and I'm not
convinced anyone knows how yet — including me.

A symptom you'll notice immediately: nearly everything is named with a short,
plain, slightly-off word. `deck`, `wand`, `wish`, `urge`, `need`, `hope`,
`feed`, `sink`, `pool`, `task`, `exec`, `coin`. Four letters,
chosen for sound and resonance as much as for precision. This is deliberate,
and it is a *technique*, not a bit. Odd names keep the concepts **soft**: a
`wand` doesn't arrive pre-loaded with decades of "Executor" baggage, so I can
keep asking what it really is — and I do, constantly. Maybe wands don't exist.
Maybe the deck is a scam. Maybe the whole thing is secretly just a *rack*. The
words are handles for moving the furniture, not labels bolted to it. Renaming
is a first-class operation here; if you get attached to the vocabulary, the
vocabulary has won and the model stops moving.

What the search keeps converging on is a single instinct: **everything here is
a way of holding work that isn't running yet, and the only real question is
what decides when it comes back.** A scheduler holds resumptions; a backend
holds outstanding requests; a buffered stream holds bytes; the smallest
awaitable holds *either a value or the work to get it*. Same shape, four sizes
— the [holding essay](@ref rt_holding) is the long version, and it ends on the
moment those four collapse into one.

And because I refuse to *only* hand-wave, the model is also written down
formally. `nxtrt/runtime.rkt` is `#lang rdf-forge` — a small homemade language
that is at once an OWL **ontology**, an Alloy-style **relational model**, and a
**temporal** spec, in one file. It describes the runtime's own `deck` / `pool`
/ `task` / `wish` / `exec` and the lifecycle an `exec` moves through (prepared
→ parked → settled → retired), states invariants as predicates, and lets the
checker search bounded **traces** — then renders straight into these docs.
That is a thread of its own: I want domain, ontological, and temporal modeling
to be *one* coherent tool, not Protégé and Alloy and TLA⁺ in three windows
that don't talk to each other.

So the open questions, right now, are roughly:

1. **Zig's buffers in C++ with coroutines.** Mostly cracked: generic *value*
   buffers, with the byte streams as the `<byte>` specialization — feeds,
   sinks, and the `hope<T>` hot path that makes the buffered case free. (See
   `src/nxtrt/value-buffers.hpp` and the [holding essay](@ref rt_holding).)
2. **A coherent unifying theory of `deck` / `wand` / `pool`.**
3. **Modeling without a pile of tools** — domain + ontology + time in a single
   language (`rdf-forge`), pointed back at the runtime it describes.
4. **Much, much more.** This list is not closed, and neither is the vocabulary.

None of this is settled. That's the point — it's a working model, in both
senses. If a name here annoys you, good: hold it loosely, like I'm trying to.
