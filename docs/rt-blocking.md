# Blocking C++ off the deck {#rt_blocking}

`nxtrt::blocking_pool` runs ordinary synchronous callables on a fixed set of
worker threads. The awaiting task, its group or pool, and its continuation
stay on their original deck. It does not host extra decks, migrate coroutines,
or replace an external build scheduler. Include `<nxtrt/blocking.hpp>` or
`<nxtrt.hpp>`.

```cpp
nxtrt::blocking_pool workers{4, 16}; // threads, admitted-call capacity
auto recipe = workers.call([input = std::move(input)]() mutable {
    return ordinary_cpp_function(input);
});
// recipe is an idea: pass it to an NXT pool, drain, or a group like settle.
// In an existing task, direct use is convenient:
auto output = co_await workers.run([input = std::move(other_input)] {
    return ordinary_cpp_function(input);
});
```

Both APIs own the callable by value and invoke it once as a mutable lvalue on a
worker. `call(fn, token = {})` produces a lazy, movable recipe returning a task;
invoking the recipe transfers its callable to that task. `run(fn, token = {})`
creates the task directly. Neither construction nor recipe invocation starts
worker work: the task must first run on its deck and acquire admission credit.
Only ordinary functions returning `void` or an owned movable value qualify;
task/hope factories and reference results are rejected. Pointer and view
results cannot be proven owning by C++: the caller must not return dangling or
worker-affine borrows. Exceptions retain their original type via
`std::exception_ptr`, and are rethrown on the deck.

## Bounds and ownership

Capacity counts queued + running + settled/not-yet-delivered calls. It is
released when the owner task delivers or discards the outcome, not when a worker
finishes. When full, `run` asynchronously waits for credit. Worker queue order
is FIFO **after admission**; there is no fairness guarantee for credit waiters,
or completion ordering across multiple workers. Pool methods and observations
are owner-deck-only. The first running call/close binds the pool to that deck.

Each call uses heap-owned callable/result storage and a nonblocking pipe; a
mutex publishes cancellation and completion. Workers only invoke ordinary code
and write notifications. A stop-shielded fd poll delegate receives readiness
through the deck's existing wand and resumes on that deck. `bell` is not used
as a cross-thread primitive. This requires a functioning POSIX fd-poll wand
(Linux io_uring/epoll, BSD/macOS kqueue), not a bare `deck::sync_wait` loop.

This bounds admitted calls, not bytes or caller tasks waiting for admission.
Use an existing bounded `pool<Idea>` over `workers.call(...)` recipes to also
bound upstream recipe consumption, frames, and outstanding outputs. There are
no permanent NXT controller tasks and no detached calls. Worker threads are
joined by `close()` or by destruction after all calls have settled.

The pool must outlive every recipe/task using it, including unstarted tasks.
The existing structured task owner (group, NXT pool, or awaited parent) retains
the frame until settlement. Captured inputs should be owned values. Borrowed
state must remain alive until **all** its calls settle; owning a callable does
not magically own its reference captures. Callable/result destruction occurs
on the deck, not necessarily on the invocation thread.

## Cancellation and teardown

There are three separate facts: stop requested, result unwanted, work settled.

- Before admission, cancellation prevents submission and returns
  `operation_cancelled` without waiting for a worker.
- After admission but before the worker commits to invocation, cancellation
  prevents user code from starting. The cancelled queue entry is settled when
  a worker dequeues it; it may wait behind an earlier running call.
- Once a worker commits to invocation under the job mutex, arbitrary blocking
  code cannot be forcibly stopped. The task remains pending until that code
  returns or throws. Its outcome is then discarded and cancellation reported.
- If cancellation and result delivery race, the mutex-protected owner check
  chooses the outcome: stop before that check wins, even if the worker already
  finished; stop after the check does not revoke the selected outcome.

The optional external `std::stop_token` is the safe foreign-thread cancellation
entrypoint: its callback touches only synchronized job state and writes the
pipe. Normal NXT task stop also cancels the job, but **do not request stop on
arbitrary NXT tasks or pools, or call `pool.stop()` from a worker**. Other NXT
stop callbacks can synchronously mutate a wand or pool on the requesting
thread.
This API does not make the rest of the runtime thread-safe. If ordinary code
supports cooperative cancellation, separately capture its own token and check
it there; the pool never interrupts that code.

`stop()` is an owner-only terminal request: reject further admission, cancel
queued jobs, and mark running results unwanted. `co_await close()` additionally
waits for owner delivery/discard of every active call, then joins the threads.
It is idempotent and stop-shielded. Close the surrounding recipe producer/NXT
pool as well; closing workers does not own or destroy those callers.

Await/join before leaving a scope, including on failure; `finally` can pair a
consumer with its NXT pool's `close()`. Destroying a service with active calls,
or forcibly destroying a parked blocking wait instead of settling its task,
fails fast rather than silently leaving a dangling continuation. Failure to
prepare a completion poll uses a synchronous settlement fallback; provision
adequate wand/task/frame capacity so this exceptional path is not normal load
control. A hung synchronous function can keep cancellation, close, and process
shutdown pending forever. Process isolation is needed for a hard kill deadline.

## Minimal realistic example

Read a build manifest with a conventional blocking C++ library while NXT keeps
HTTP/supervision tasks responsive. The filesystem operation and its stream
lifetime stay entirely inside the ordinary callable; its path and output are
owned. For a campaign, feed these recipes through a bounded NXT pool.

```cpp
#include <nxtrt.hpp>
#include <fstream>
#include <iterator>
#include <stdexcept>

nxtrt::task<std::string> read_manifest(nxtrt::blocking_pool & workers,
                                      std::string path) {
    co_return co_await workers.run([path = std::move(path)] {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open " + path);
        std::string text{std::istreambuf_iterator<char>{file}, {}};
        if (file.bad()) throw std::runtime_error("cannot read " + path);
        return text;
    });
}

int main() {
    nxtrt::blocking_pool workers{4, 16};
    nxtrt::runtime rt;
    auto manifest = rt.run([&] {
        return nxtrt::finally(read_manifest(workers, "manifest.json"),
                              [&] { return workers.close(); });
    });
    // Use manifest on the owner thread; all worker calls/threads are settled.
}
```

The lambdas are ordinary callables returning values or tasks from named
functions. None is a capturing coroutine lambda.

## Serialized stateful libraries (for example DuckDB)

A `blocking_pool{1, capacity}` has exactly one persistent worker OS thread.
Thus admitted calls are FIFO, serialized, **and** thread-affine for that pool's
lifetime. A multiple-worker pool with a mutex would provide only serialization,
not affinity. No DuckDB dependency or actor/executor layer is required.

Keep a small service record outside all caller frames. Initialize its connection
inside a call on the one-worker pool; submit query callables carrying owned SQL
and parameters; return materialized owned rows, not connection/result borrows.
After stopping producers and joining all query tasks, submit a cleanup callable
that destroys the connection **on the worker**, await it shielded (for example
via `finally`), and only then close the pool. Do not call `stop()/close()` before
submitting that cleanup, because it prevents cleanup admission too.

The pool does not own your service record and does not automatically initialize
or destroy it on a worker. Explicit worker-side destruction matters because
the callable and its captures are destroyed on the deck. Do not let a captured
`unique_ptr<Connection>` implicitly delete a thread-affine connection there.
No stateful-owner API is added: lifecycle calls plus one persistent worker give
the required affinity without introducing a second ownership framework.
