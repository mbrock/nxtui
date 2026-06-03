# RFC 0013: Runtime Env Core Fields {#rfc_runtime_env_core_fields}

Status: new

## Summary

The runtime environment should keep hot runtime context in direct fields:

```text
current_deck
current_firm
current_wand
current_task_id
current_promise
```

The existing vector/snapshot environment can remain for rare dynamic bindings
such as tracing context, games, and user-level ambient values. Frame
allocation, wish realization, and task registration should use fixed core
fields instead of searching the generic environment.

## Motivation

The current [env.hpp](../../src/nxtrt/env.hpp) already has two direct fields:

```cpp
deck * current_deck;
detail::promise_base * current_promise;
```

Everything else is represented as typed entries in an immutable snapshot list.
That is elegant for rare ambient bindings. It is not the right path for the hot
runtime spine.

Upcoming RFCs need fast access to:

- the current firm for coroutine frame allocation;
- the current wand for wish realization;
- the current task id for completion routing and diagnostics;
- the current promise or task table row for result routing and stop state.

Keeping these as direct fields makes the runtime's core relationships obvious
and avoids treating scheduler mechanics like user-level dynamic context.

## Current Shape

Current environment pieces:

- `runtime_env` owns an immutable snapshot of typed entries.
- `detail::env_guard` installs `current_deck` and `current_promise` for one
  coroutine resume.
- `firm_key` stores the current firm as a generic environment binding in
  [task.hpp](../../src/nxtrt/task.hpp).
- `current_deck()` reads `runtime_env::current_deck`.
- `deck::current_task_id()` reads the id from `current_promise`.
- `current_wand()` is reached through `current_deck()->current_wand()`.

This works, but it makes hot runtime state a mixture of direct fields,
promise fields, deck fields, and generic env entries.

## Proposal

Extend `runtime_env` with core fields:

```cpp
struct runtime_env {
    deck * current_deck = nullptr;
    firm * current_firm = nullptr;
    wand * current_wand = nullptr;
    task_id current_task_id = {};
    detail::promise_base * current_promise = nullptr;

    entry_snapshot entries = empty_entries();
};
```

`env_guard` should install all core fields for a resume. Copying an environment
from parent to child should copy rare entries but clear resume-specific fields,
as it does today for deck and promise.

`with_env<Key>` remains the mechanism for rare dynamic bindings. `firm` stops
being stored as a generic binding once the core field exists.

## Hot Users

Frame allocation:

- promise allocation consults `current_firm`;
- allocation diagnostics include `current_task_id`.

Wish realization:

- awaiting a wish reads `current_deck`, `current_wand`, `current_task_id`, and
  the task await slot;
- the wand does not rediscover itself through the deck unless that remains the
  chosen ownership API.

Task registry:

- `current_task_id` is direct, not recovered from `current_promise`;
- debug and tracing can report the current task without promise peeking.

Firm operations:

- `current_firm()` reads a direct field;
- `with_firm` installs the direct field while running the body.

## Why Keep Generic Env?

The generic environment is still useful for values that are not part of the
runtime's core execution model:

- trace context and current span;
- game context;
- request-scoped metadata;
- user-provided capabilities;
- rare dynamic settings.

Those values benefit from snapshot copy and typed keys. They are not consulted
on every frame allocation, task resume, or wish await.

## Invariants

Core fields are valid only while a task is being resumed or while code is
running under an explicit runtime/root guard.

Copying a runtime environment for a child task copies generic entries but does
not copy a stale `current_deck`, `current_promise`, or running-task identity.

The current firm field must be updated by firm-scope helpers and restored when
the scope exits.

The current wand field must agree with the deck/wand pairing used for the
current pump round.

## Relationship To Other RFCs

[RFC 0002](rfc-0002-firm-frame-arenas.md) needs `current_firm` in the promise
allocation path.

[RFC 0003](rfc-0003-deck-task-registry.md) needs `current_task_id` as a hot
identity.

[RFC 0004](rfc-0004-wand-completion-routing.md) and
[RFC 0009](rfc-0009-wishes-urges-and-provided-buffers.md) need a direct path
from an awaited wish to the current task, firm, and wand.

## Open Questions

- Should `current_wand` be stored directly, or should `current_deck` remain the
  only owner of the active wand pointer?
- Does `current_promise` remain a core field after task-table migration, or
  does it become a derived pointer from `current_task_id`?
- How should root synchronous code install core fields before the first task is
  resumed?
- Should `firm_key` remain temporarily as a compatibility path during
  migration?
- What parts of this change should be reflected in `runtime.rkt`?

## References

- [RFC 0002: Firm Frame Arenas](rfc-0002-firm-frame-arenas.md)
- [RFC 0003: Deck Task Registry and Task IDs](rfc-0003-deck-task-registry.md)
- [RFC 0004: Wand Completion Routing without exec Hub](rfc-0004-wand-completion-routing.md)
- [Runtime Overview](../../docs/rt-overview.md)
- [env.hpp](../../src/nxtrt/env.hpp)
- [task.hpp](../../src/nxtrt/task.hpp)
- [deck.hpp](../../src/nxtrt/deck.hpp)
- [wand.hpp](../../src/nxtrt/wand.hpp)
