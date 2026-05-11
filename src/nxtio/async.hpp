#pragma once

// Public async facade.
//
// This is the map of nxt's async vocabulary. It intentionally includes the
// small core aliases first, then the higher-level building blocks that give
// those aliases meaning in the UI/runtime system.
//
// Core execution:
//   task<T>             coroutine result
//   io_scheduler        executor used for timers, polling, and detached work
//   queue<T>, event     low-level libcoro synchronization primitives
//   sync_wait(...)      bridge from synchronous main/test code into async code
//   when_all(...)       await a collection of tasks
//
// Structured lifetime:
//   scope<Context>      cancellation tree + scheduler + contextual values
//   scope.subscope()    child lifetime inheriting cancellation and context
//   scope.spawn(...)    collect scoped tasks
//   scope.all()         run scoped tasks to completion
//   scope.any()         run until one completes, then cancel the scope
//
// Event delivery:
//   Signal<T>           single-waiter, unbuffered event signal
//   Publisher<T>        copyable write endpoint for a Signal<T>
//   signal.next(stop)   cancellable wait, suitable for scoped races
//   event_queue<T>      buffered cancellable queue, under nxt::io
//
// UI process layer:
//   UIRuntime           application host: scheduler, input, signals, rendering
//   Self                process capability facade over scope<ProcessContext>
//   ProcessHandle       parent-side lifetime handle for a spawned process
//
// System bridges:
//   SignalPipe          OS signal delivery through a pollable fd
//   PtySession          PTY-backed subprocess session
//
// Lower-level implementation headers should include `async-core.hpp` when they
// only need task/queue/event/scheduler aliases. Application code can include
// this header when it wants the whole async vocabulary in view.

#include "nxtio/async-core.hpp"

#include "nxt/signal.hpp"
#include "nxtio/event-queue.hpp"
#include "nxtio/process.hpp"
#include "nxtio/scope.hpp"
#include "nxtio/signal-pipe.hpp"
#include "nxtio/subprocess.hpp"
