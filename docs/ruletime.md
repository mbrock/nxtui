# Protocols, Games, Coroutines, and the Algebra of Unfolding

One of the recurring frustrations in coroutine systems is the desire to
describe processes that do more than simply "eventually return a value".

A task:

cpp task<T>

describes:

text eventually T

But many real processes have richer temporal structure:

text become ready emit values emit more values emit more values eventually
settle

Examples include:

- child processes
- websocket sessions
- HTTP request bodies
- file transfers
- event streams
- services
- behavioral threads
- protocol state machines

Traditionally we model these with ad hoc combinations of:

text future generator channel callback state machine

The result is usually awkward.

## A Different View

Suppose we stop thinking about these things as computations and instead think
of them as temporal languages.

For example:

text ready then many<Card> then expected<int,E>

This can be written:

cpp seq< ready, many<Card>, expected<int,E> >

which is simply the regular language:

text ready Card\* result

This is simultaneously:

- a protocol
- a session type
- a parser
- a behavioral thread
- a temporal logic formula

depending on how we interpret the symbols.

## The Jack

A useful concrete example is:

cpp jack<T,V>

which can be understood as:

text ready >> many<V> >> expected<T,E>

or:

text boot emit\* settle

A child process is naturally:

text spawned >> many<output_chunk> >> exit_status

A websocket:

text open >> many<message> >> close_reason

A service:

text ready >> many<event> >> shutdown_result

The key observation is that EOF and final result become distinct concepts.

The feed may end:

text no more emissions

while the final deed still remains:

text how did the resident settle?

This is a surprisingly natural model.

## Behavioral Threads as Protocols

Behavioral programming introduces sync points:

cpp sync(...)

A sync point is not an event.

It is a declaration about future events:

text I request A I wait for B I block C

A behavioral thread is therefore already a protocol automaton.

The traditional implementation uses coroutine bodies:

cpp while (...) { sync(...) }

But there is another possibility.

A protocol expression:

cpp A >> many(B) >> C

already describes a state machine.

The protocol itself can become the behavioral rule.

The rule is no longer an opaque coroutine.

It becomes an inspectable temporal object.

## The Surprising Connection to Parsing

Parser combinators operate on languages.

For example:

text A B\* C

A protocol:

text A B\* C

has exactly the same shape.

The difference is only the alphabet.

A parser consumes:

text tokens

A protocol consumes:

text events

A behavioral rule constrains:

text events

A session type describes:

text interactions

The algebra is the same.

Operators such as:

text seq choice many optional until

already exist in parsing.

They may also serve as protocol combinators.

## Games as Temporal Logic

A parser recognizes a language.

A behavioral rule imposes a language.

This distinction is important.

A rule:

text boot >> many(work) >> shutdown

does not merely recognize histories.

It constrains histories.

It says:

text work may not occur before boot shutdown may not occur before work

Such a rule may contain only a few states and yet influence the entire
lifetime of the system.

This begins to resemble temporal logic.

Each rule contributes:

text a language of acceptable futures

and the game computes:

text their lawful overlap

In other words:

text conjunction

of temporal constraints.

This is why behavioral programming often feels more like logic than
scheduling.

## A Denotational Semantics

One attractive direction is to define a semantic mapping:

text ⟦ protocol expression ⟧

into behavioral-thread structures.

For example:

text ⟦ seq(A,B) ⟧

becomes a thread that waits for A and then B.

text ⟦ many(A) ⟧

becomes a looping thread.

text ⟦ choice(A,B) ⟧

may become a nested game representing alternative futures.

The protocol language becomes syntax.

Behavioral threads become semantics.

This is exactly the sort of relationship that parser combinators have to
automata.

## Why This Is Not Crazy

This may sound ambitious, but all of the ingredients already exist.

We already know how to build:

- regular expressions
- parser combinators
- automata
- session types
- behavioral threads
- coroutine state machines

The observation is simply that these structures are much closer than they
first appear.

A coroutine with:

cpp co_yield co_return

already defines a temporal language.

A behavioral thread already defines a temporal language.

A parser already defines a temporal language.

The proposal is not to invent a new mathematical object.

The proposal is to recognize a common one.

## Why C++ Is Surprisingly Suitable

At first glance this sounds more like Haskell than C++.

And indeed, prototyping the denotational semantics in Haskell may be an
excellent idea. Haskell is exceptionally good at:

- algebraic data types
- interpreters
- parser combinators
- denotational semantics
- protocol experimentation

However, C++ possesses a surprising advantage.

Modern C++ coroutines expose explicit control over:

- suspension
- resumption
- allocation
- frame ownership
- promise types

At the runtime level we can directly realize:

text task feed jack firm game

as concrete objects with explicit memory ownership and structured lifetimes.

This means that C++ may be unusually well-suited for realizing the semantics
even if another language is better suited for discovering them.

The protocol algebra may be easiest to invent in Haskell.

The runtime institutions may be easiest to build in C++.

Those two activities complement each other rather than compete.
