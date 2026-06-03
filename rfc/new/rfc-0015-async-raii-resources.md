# RFC 0015: Async RAII Resources {#rfc_async_raii_resources}

Status: new

## Summary

A task forked into a firm can represent an async RAII resource.

The resource is owned by the firm. It may perform asynchronous construction,
publish readiness, emit a feed or capability while alive, and perform
asynchronous teardown when the firm stops or exits.

The slogan:

```text
task in a firm = scoped async resource
```

## Motivation

Synchronous RAII works because construction, use, and destruction are scoped.
Async resources need the same discipline, but their setup and teardown may
suspend.

Examples:

- a provided buffer group registered with a wand;
- a subprocess or pty session;
- a multishot accept producer;
- a file watcher;
- a protocol session that emits frames until stopped.

These are not detached tasks. They are resources located inside a firm. The
firm owns their lifetime and cancellation.

## Phases

An async resource task often has phases:

```text
boot   acquire/register/start
ready  publish capability or feed
emit   produce events, chunks, loans, or status
quit   unregister/drain/cancel/release
```

The phase names are provisional. The important part is that readiness is not
the same as final result. A resource may become ready, emit values for a long
time, and only later return or throw during teardown.

## Shape

One possible shape:

```cpp
auto bg = fork(buffer_group_resource, storage);
auto ready = co_await bg.ready();
auto loan = co_await ready.recv_some(fd, max);
```

Another:

```cpp
task<resource_result> run_resource(resource_port port);
```

where `resource_port` is how the task publishes readiness and emitted items to
the owning firm.

The exact API is open. The first design should avoid requiring a task to return
`pair<feed<V>, deed<T>>` by hand at every call site. That type expresses the
shape, but the runtime should provide a clearer resource handle.

## Buffer Groups

[RFC 0010](rfc-0010-firm-buffer-groups-and-io-land.md) is the motivating
example.

A firm-owned buffer group may need to:

- allocate or borrow memory;
- register buffers with a wand/backend;
- publish a group handle once registration succeeds;
- hand out buffer loans while alive;
- stop new loans when the firm stops;
- wait for outstanding loans to return or be cancelled;
- unregister from the backend;
- release memory back to the firm.

That is async RAII. The group is not just a struct; it is scoped work with
setup, live service, and teardown.

## Relationship To Feeds

Many async resources emit feeds:

```text
accept resource -> feed<accepted_socket>
recv resource   -> feed<byte_loan>
watch resource  -> feed<event>
```

This overlaps with [RFC 0011](rfc-0011-multishot-wishes-as-feeds.md). The
difference is emphasis: RFC 0011 describes the multishot operation as a feed,
while this RFC describes the whole producer as a scoped resource in a firm.

## Invariants

An async resource belongs to one firm.

The firm may not finish destruction until the resource has either completed
normally or completed its cancellation/teardown path.

Readiness publication happens at most once.

After a firm stops, a resource should stop issuing new external capabilities
and should drive outstanding loans, feeds, or child work toward settlement.

## Open Questions

- What is the minimal readiness primitive: deed, feed item, channel, or a
  dedicated resource handle?
- Does a resource task return its final result through an ordinary deed, or
  through a resource-specific handle?
- How should resource teardown interact with time budgets from
  [RFC 0014](rfc-0014-idea-algebra.md)?
- Can buffer groups, subprocesses, and multishot producers share one resource
  abstraction?

## References

- [RFC 0010: Firm Buffer Groups and I/O Land](rfc-0010-firm-buffer-groups-and-io-land.md)
- [RFC 0011: Multishot Wishes as Feeds](rfc-0011-multishot-wishes-as-feeds.md)
- [RFC 0014: Idea Algebra](rfc-0014-idea-algebra.md)
- [Runtime Overview / Firms](../../docs/rt-overview.md)
- [The nxtrt runtime, as a story about holding work](../../docs/rt-holding.md)
- [task.hpp](../../src/nxtrt/task.hpp)
