# Runtime consolidation notes

The application/runtime surface has moved from the old `nxtio` stack onto
`nxtrt`. `libcoro` has been removed from the Meson build and from
`subprojects`, and the remaining `nxtio` sources have been deleted. The old
custom task prototype is gone; its useful ideas now belong in `nxtrt::env`,
groups and pools, and explicit UI/runtime capabilities.

## Current Shape

`src` owns the coroutine substrate:

- `nxtrt::task<T>` for lazy coroutine tasks.
- `nxtrt::deck` for pumpable execution.
- `nxtrt::wand` implementations for platform waiting.
- Groups (`settle`, `settle_range`, `when_all`, `wait_any`, `with_timeout`),
  bounded `pool<Idea>` evaluation, and `drain` for feeds of ideas.
- DNS, HTTP, TLS, and socket experiments.
- Subprocess wishes for piped children, pty children, waits, and signals:
  pidfd-based on Linux, pid-based with `EVFILT_PROC` on kqueue.
- OpenAI Responses request JSON and small SSE streaming clients.
- Core terminal input types and parsing in `src/nxtui/input.hpp`.
- The default `nxtllm` executable with streaming and a pool-backed tool loop.

The old application stack is no longer in the tree. The former `src/nxt/ai`
LLM stack has also been removed; the surviving LLM code lives in
`src/nxtai`.

## Migration table

| Old surface | New target | Notes |
| --- | --- | --- |
| `nxt::task<T>` | `nxtrt::task<T>` | Start with leaf code that does not expose scheduler handles. |
| `nxt::scheduler` | `nxtrt::deck` + `nxtrt::wand` | Keep the host pumpable so terminal and Emacs embeddings can own the event loop. |
| `scheduler.yield_for(d)` | `nxtrt::op::timeout::after(d)` or `with_timeout` | Keep sleep/yield as runtime methods at the app boundary. |
| `scheduler.poll(...)` | `nxtrt::op::poll*` | Convert call sites once they are inside an `nxtrt::task`. |
| `nxt::queue<T>` | `nxtrt::wire<T>` | Done. Used for UI input, resize, and tool streams. |
| `nxt::event` | `nxtrt::bell` | Done. Used for damage notifications and small UI coordination points. |
| `nxtio/input.hpp` | `nxtui/input.hpp` | Done. The compatibility include has been removed. |
| `nxt::latch` | `settle` / `when_all` over the jobs, or a small latch | Prefer awaiting a group; add a latch only for true countdown cases. |
| `spawn_detached` | a job in a group, or an idea fed to `drain` / `pool<Idea>` | There is no fork or ambient spawning; work is owned by the group or pool it is handed to. |
| `nxt::scope` | a group stop predicate + UI capabilities | Cancellation is per task; a group stops and drains its own jobs. The richer yard-style UI facade is still being rebuilt on top. |
| `nxtio/net` | `src` HTTP/TLS/DNS | Done. The OpenAI streaming path uses the new HTTP client directly. |
| old shell/pty subprocess helpers | `nxtrt::op::spawn_pty` + `nxtrt::pty::session` | PTY processes are now pidfd-owned wishes and can render through vterm without a separate output mailbox. |
| old LLM entry point | `src/nxtai/nxtllm.cpp` | Ported. Streaming and tool continuations use the ordinary runtime; terminal output is an observer without the old HUD/tool UI runtime path. |

## Firm API migration

Firms, `with_firm`, `fork` / `join`, and deeds have been removed (see
[RFC 0019](../rfc/new/rfc-0019-firms-without-bodies.md)). Port code by the
shape of its concurrency:

- A scope-only firm (cancellation or lifetime, no children) becomes a plain
  awaited task. Read stop through `current_stop_token()`, `stop_requested()`,
  or `throw_if_stop_requested()`, which see the running task's stop.
- A fixed fork/fork/join becomes `settle(std::tuple{a, b}, execution)`, which
  returns `std::tuple<outcome<T>...>`, or `when_all` / `wait_any` when the
  usual aggregation fits. Elements are tasks, created before entering the
  group; call task factories to obtain those tasks first.
- A firm policy that stopped siblings becomes a callable stop predicate:
  `fail_fast_group`, `first_success_group`, `first_completion_group`,
  `primary_group` (a primary job plus companions), or a custom callable
  `bool(std::size_t index, bool failed) noexcept`.
  `all_group` is the default and does not stop siblings.
- A loop of forks over a range becomes `settle_range(range, execution)`, returning
  outcomes in range order.
- An open-ended stream of forked work becomes a feed of ideas run by
  `drain(ideas, capacity)`, or a `pool<Idea>` over a `pool_land<Idea>` when the
  results are needed.
- `catching_deed` / `.cope()` collection becomes reading the `outcome<T>`
  values a group returns.

Fixed groups now directly own tuple/vector tasks and observe completion through
synchronous completion links, rather than lowering through pool recipes or
wrapper tasks.
There is no `group_recipe`, separate intermediate results tuple, or public deed.
The task-only `settle`, `when_all`, and `wait_any` APIs remain unchanged; pools
remain streaming bounded idea-factory evaluators.

Stop rules see promise success/failure at final suspension. Results stay in
promises until all started tasks have drained, then move into returned outcomes.
Initial extraction errors become exception outcomes without changing the rule's
decision; subsequent moves of the result tuple/vector can throw after drain.
Outside cancellation stops and drains the tasks before propagation.

Root entry takes a factory directly: `deck.sync_wait(fn, args...)`,
`runtime.run(fn, args...)`, `run_with_kqueue(fn)`, or the io_uring
`nxtrt::run(fn)`.
The factory is called inside the runtime environment and kept alive while
its task runs.

## Completed Slices

1. Add `nxtrt` wire and bell primitives. Done as bounded
   `nxtrt::wire<T>` and manual-reset `nxtrt::bell`.

2. Introduce a runtime facade beside terminal UI helpers. Done as
   `nxtrt::runtime`: it owns a `deck`, platform `wand`, root-task run
   entrypoint, damage bell, input wire, resize wire, and `sleep`.
   `nxtrt::terminal_app` and the runtime demos layer terminal/compositor
   ownership on top of it.

3. Port buffer and HTTP helpers to `nxtrt::task`. Done in `src/nxtrt`.
   Keep request/response data structures free of runtime dependencies.
   `src/nxtai/responses_request.hpp` is the model for that split.

4. Make OpenAI streaming use `src` networking. Done for the one-shot
   `nxtllm` path: it connects over `nxtrt` TCP/TLS, reads HTTP/SSE, and
   writes text deltas to stdout.

5. Port tool execution after streaming works. Done in `src/nxtai/agent.hpp`:
   the CLI connects completed Responses items to `tool_batch.hpp` and resumes
   the model with tool outputs. `responses_stream.hpp` handles stream events
   independently of terminal rendering. The old runtime UI wrapper is gone.

6. Re-enable `nxtllm` on the runtime. Done in
   `src/nxtai/nxtllm.cpp`: the executable builds by default, parses CLI
   options, enters `nxtrt::runtime`, and streams Responses text over the `src`
   HTTP/TLS stack.

7. Use PTYs for live tool surfaces. Started with `nxt-shell-scope-demo`: the
   command runs through `spawn_pty`, feeds a `vterm` session, and renders as a
   TUI surface. Cgroup sampling reads files through `openat`/`read_some`
   wishes and batches each sample with `when_all`.

## Compatibility strategy

Do not restore a compatibility alias layer. The old libcoro task and
`nxtrt::task` have different ownership and pump semantics, and a dual alias
layer would hide the hard parts. Prefer explicit ports:

- Move dependency-light data types first.
- Port leaf async functions next.
- Keep old UI demos only when they teach an unported subsystem; delete them
  once the runtime version covers the same behavior.
- Treat a clean default Meson build as the guardrail for runtime-only work.

## Remaining Work

The core migration is done. The remaining work is product shape:

1. Rebuild any richer interactive `nxtllm` UI as a separate observer over
   stream events instead of reviving the old UI runtime.
2. Decide which demos still carry their weight after the runtime consolidation.
3. Keep request construction and JSON parsing runtime-neutral.
4. Keep the default Meson build as the guardrail for runtime-only work.
