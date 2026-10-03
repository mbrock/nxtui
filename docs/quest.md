# The quest {#quest}

nxt is a set of working libraries, but the libraries are not the point. The
point is a search for one model of concurrent, effectful computation that is,
all at once:

- **coherent**: the pieces actually fit, instead of sitting in adjacent
  layers and translating between each other;
- **correct**: you can say precisely what it does, and check that it does;
- **efficient**: the common case, where the answer is already here, costs
  nothing.

Most systems get two of these. I want all three, and I am not sure anyone
knows how yet, including me. This page describes what the search is looking
for, what it has found so far, and what is still open. The rest of the
documentation describes what exists today; this page is about why it looks
the way it does, and where it is going.

[TOC]

## Soft words {#quest_words}

The first thing you will notice is the vocabulary: `deck`, `wand`, `wish`,
`urge`, `need`, `hope`, `feed`, `sink`, `land`, `farm`, `pool`, `idea`,
`coin`. Short, plain, slightly off. This is a technique.

A word like "executor" arrives carrying decades of other people's designs.
Once a concept has that kind of name, it stops moving: you start asking what
an executor is supposed to do, rather than what this thing actually is. An
odd name keeps the concept *soft*. A `wand` has no tradition to live up to,
so I can keep asking what it is, and the answer keeps changing. Maybe wands
don't exist. Maybe the deck is secretly just a buffer. The words are handles
for moving the furniture, not labels screwed onto it.

So renaming is a normal operation here, and several names have already been
retired along with the ideas they named (`firm`, `deed`, `game`). Hold the
vocabulary loosely. If it starts to feel permanent, it has won, and the model
has stopped moving.

## One shape, four sizes {#quest_holding}

The search keeps returning to a single observation:

> Concurrency is the art of holding work that is not running right now, and
> the only real question is what decides when it comes back.

A single thread does one thing at a time. As soon as there are two things,
something has to hold the one that is not running and hand it back later. Each
layer of the runtime is one answer to "where is the held work kept, and what
releases it?":

| Holder | Holds | Releases it when |
| --- | --- | --- |
| deck | ready tasks | the next round runs, in FIFO order |
| wand | parked tasks, keyed by pending operation | the platform completes the operation |
| feed / sink | values (often bytes) | a reader takes them / a drain writes them |
| hope | a value, or the task that will produce it | immediately, or when that task finishes |

These are the same shape at four sizes: a buffer with a release policy. A
scheduler is a buffer whose release policy got clever; a buffer is a
scheduler whose release policy stayed trivial. @ref rt_holding tells this
story at length, from the code up.

The shapes are not yet unified in the types. A wish always suspends, even
when the kernel could have answered at once, while a `hope` gets to say
"already here." Giving wishes an honest fast path would let a buffered feed
*be* a wand, and `hope` would dissolve into the one awaitable everything
returns: maybe already here, otherwise suspend. That merge is the clearest
next step the code is shaped toward.

## Holding work in the heap {#quest_wisp}

The same question has a second answer at a different scale. If held work is
just data, it does not have to stay in memory, or in this process.

C++ coroutine frames can't be saved: they are opaque, full of native pointers,
and tied to one binary. So @ref wisp keeps a guest program's entire control
state in its own heap instead: the evaluator's stack, delimited continuations,
the pending request, the handler that will resume it. Native coroutines in
the host only do the waiting. At any `await`, everything the program needs to
continue is heap data, which is why a running Wisp program can be written to
a tape, restored in another process, inspected, or forked.

The rule is strict: guest control state stays in the heap. It turns
"suspended computation" from a runtime mechanism into a value you can hold in
your hand, and that is the version of held work I find most interesting. It
is also where agents come in. An agent working inside its own Lisp machine
can define helpers, inspect its own suspended requests, change code, and
continue, and the whole session is a file.

## Checking the model {#quest_model}

I don't want the model to exist only as prose, so it is also written down
formally. `nxtrt/runtime.rkt` is written in `#lang rdf-forge`, a small
language made for this project. One file is at the same time:

- an **ontology**: the runtime's concepts as classes and properties, exportable
  as RDF/OWL;
- a **relational model**: signatures and invariants in the style of Alloy;
- a **temporal specification**: how executions move from prepared to parked
  to settled to retired, checked over bounded traces.

`make spec` checks that every scenario in it is satisfiable and that every
claimed property holds, printing a counterexample trace when one doesn't.
When the runtime's lifecycle rules change, the model changes with them.

The goal behind this is to make domain modelling, ontology, and temporal
reasoning *one* activity with one tool, instead of Protégé, Alloy, and TLA⁺
in three windows that don't talk to each other.

## Open questions {#quest_open}

Roughly, as of now:

1. **Zig's buffers, with coroutines.** Mostly answered. Generic value
   buffers, with byte streams as the `std::byte` case, and `hope<T>` making the
   buffered path free. See `feed`, `sink`, and @ref rt_holding.
2. **A unified theory of deck, wand, and pool.** Open. The eager-wand
   merge above is the next concrete step.
3. **Structured concurrency without a nursery object.** Partly answered.
   Groups own exactly the tasks they are given and drain them before
   returning; pools bound a stream of work. Firms (a scope object with fork
   and join) were tried and removed. What a *team* of long-lived
   cooperating jobs should look like is still open; see
   [the concurrency direction note](rt-concurrency-direction.md).
4. **Effects you can save.** Wisp checkpoints at timers today. Saving at
   arbitrary effects needs a log of effect results and a policy for rebinding
   live resources (sockets, processes) on restore.
5. **Modelling without a pile of tools.** In progress, through `rdf-forge`
   and its use on the runtime itself.
6. **More.** The list is not closed, and neither is the vocabulary.

None of this is settled. It is a working model in both senses of the word. If
a name annoys you, good: hold it loosely, the way I am trying to.

The @ref runtime_rfcs "runtime RFCs" are the design notebook where these
questions are worked on; some describe code that exists, some are
speculation.
