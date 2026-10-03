#pragma once

#include "nxtrt/task.hpp"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) \
    || defined(__OpenBSD__) || defined(__DragonFly__)
#define NXT_RT_HAS_KQUEUE 1
#else
#define NXT_RT_HAS_KQUEUE 0
#endif

#include <coroutine>
#include <memory>
#include <type_traits>
#include <utility>

namespace nxtrt {

/// True when `kqueue_wand` is available (macOS and the BSDs).
inline constexpr bool has_kqueue_wand = NXT_RT_HAS_KQUEUE != 0;

#if NXT_RT_HAS_KQUEUE

namespace detail {
class kqueue_impl;
} // namespace detail

/// kqueue-backed wand for macOS and the BSDs; the default there.
///
/// Each awaited wish becomes one hub-stored execution record. Kevent
/// `udata` points at that record while variant phases make prepared,
/// parked, settled, delayed-delete, and retired states explicit. The
/// implementation lives in kqueue.cpp: the wand interface is already
/// type-erased, so none of it needs to be compiled into every user of this
/// header.
///
/// Batching: like epoll, `wave` tries each queued wish's syscall at once on
/// the deck's thread. Socket send/receive use `MSG_DONTWAIT`; generic I/O,
/// connect, and accept temporarily enable `O_NONBLOCK` and restore it after
/// each syscall. The temporary flag is visible to other threads sharing
/// that open-file description. Accepted sockets honor the wish's flags.
/// A wish that would block adds a one-shot `EV_ADD` change; changes from
/// one wave, including cancellation deletes, go to the kernel in one
/// `kevent` call. A retried I/O call that still reports `EAGAIN` is armed
/// again.
///
/// FD-readiness wishes use privately duplicated fds so waiters on the same
/// fd and direction do not conflict. `openat`
/// and spawns are synchronous syscalls, `asynchronous_files()` is false,
/// and the Linux-only file wishes do not exist here. Children are named by
/// pid; `wait_child` registers `EVFILT_PROC` `NOTE_EXIT`, falling back to a
/// short retry timer while a child is mid-exit, and reads the status with
/// `WNOWAIT` (see `child_handle`). `poll` reports `POLLIN`/`POLLOUT` and
/// maps `EV_EOF`/`EV_ERROR` to `POLLHUP`/`POLLERR`.
///
/// Timers: `timeout` and `poll_until` use `EVFILT_TIMER` with
/// `NOTE_NSECONDS` where available, or milliseconds rounded up otherwise;
/// a zero duration fires after the smallest positive delay in those units.
///
/// Cancellation: a queued wish settles with `operation_cancelled` at the
/// next wave. A registered wish is settled with `operation_cancelled` at
/// the next wave, which stages `EV_DELETE` for its events; its record is
/// retired only after those deletes are applied and one more receive pass
/// has drained any stale events naming it.
class kqueue_wand final : public wand
{
public:
    kqueue_wand();
    ~kqueue_wand() override;

    kqueue_wand(const kqueue_wand &) = delete;
    kqueue_wand & operator=(const kqueue_wand &) = delete;
    kqueue_wand(kqueue_wand &&) = delete;
    kqueue_wand & operator=(kqueue_wand &&) = delete;

    void suspend(wait_token token, need task) override;
    void cancel(wait_token token) override;
    void wave(deck & d) override;
    /// Applies pending changes, handles ready events without blocking, and
    /// requeues settled tasks.
    void poll(deck & d);
    /// Blocks until at least one event arrives, then handles ready events.
    void wait(deck & d);

    /// Pump the deck and the kqueue until `root` has finished.
    ///
    /// Throws `runtime_error` ("deadlock") if `root` is unfinished while
    /// nothing is ready, staged, or registered.
    void run_until_done(deck & d, std::coroutine_handle<> root);

    template<typename T>
    void run_until_done(deck & d, task<T> & root)
    {
        run_until_done(d, std::coroutine_handle<>{root.handle()});
    }

    /// Settles the parked execution `token` with a scalar `result` (a count,
    /// fd, or negative errno) and requeues its task.
    void complete(deck & d, wait_token token, int result);
    /// Requeues the task parked on `token` without storing a result or
    /// settling its record, so the await throws `runtime_error`. Not for
    /// normal use.
    void fulfill(deck & d, wait_token token);

protected:
    wait_token prep(
        deck & d,
        detail::promise_base & promise,
        detail::prepared_wish packet) override;

private:
    std::unique_ptr<detail::kqueue_impl> impl_;
};

/// Runs an already-created `root` task on a fresh `kqueue_wand` and deck.
///
/// Prefer the factory overload: a task captures the runtime environment
/// when it is created, and only the factory overload creates the root
/// inside the root environment.
template<typename T>
[[nodiscard]] inline T run_with_kqueue(task<T> root)
{
    auto wand = kqueue_wand{};
    auto d = deck{&wand};
    d.start(root);
    wand.run_until_done(d, root);

    if constexpr (std::is_void_v<T>) {
        std::move(root).result();
    } else {
        return std::move(root).result();
    }
}

/// Creates the root task by calling `fn()` inside a fresh `kqueue_wand`
/// deck, drives it to completion, and returns its result. `fn` outlives the
/// task, so a capturing coroutine lambda is safe here.
template<task_factory Fn>
[[nodiscard]] inline task_result_t<std::invoke_result_t<Fn>>
run_with_kqueue(Fn && fn)
{
    auto wand = kqueue_wand{};
    auto d = deck{&wand};
    auto root_env = runtime_env{};
    auto root_guard = detail::env_guard{root_env, &d, nullptr};
    // The factory outlives its task: a capturing coroutine lambda's frame
    // refers to the closure object.
    auto factory = std::decay_t<Fn>{std::forward<Fn>(fn)};
    auto root = std::invoke(factory);

    d.start(root);
    wand.run_until_done(d, root);

    if constexpr (std::is_void_v<task_result_t<std::invoke_result_t<Fn>>>) {
        std::move(root).result();
    } else {
        return std::move(root).result();
    }
}

#endif

} // namespace nxtrt
