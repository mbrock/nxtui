#pragma once

/**
@namespace nxtrt

A single-threaded coroutine runtime with pluggable I/O backends.

Code is written as lazy `task<T>` coroutines. A @ref nxtrt::deck "deck"
resumes ready tasks in rounds on one thread; awaiting a task queues it and
resumes the awaiting task when it finishes, and stop requests flow from
awaiting task to awaited task. Groups (`settle`, `when_all`, `wait_any`,
`with_timeout`) run fixed sets of tasks concurrently and always drain them.

I/O is expressed as wishes (`op::read_some`, `op::timeout`, ...): plain
operation values that the deck's @ref nxtrt::wand "wand" (io_uring, epoll or
kqueue) prepares, submits and completes. On top of that sit buffered value
streams (`feed`, `sink`, `bytefeed`, `bytesink`), bounded pools of `idea`s,
a blocking pool for synchronous work on worker threads, and protocols such
as filesystem access, networking, TLS, HTTP and subprocesses.

Start with @ref rt_overview "the runtime overview", then
@ref rt_holding "holding work", @ref rt_pool "pools" and
@ref rt_blocking "blocking work".
*/
