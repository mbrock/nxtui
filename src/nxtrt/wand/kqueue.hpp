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

inline constexpr bool has_kqueue_wand = NXT_RT_HAS_KQUEUE != 0;

#if NXT_RT_HAS_KQUEUE

namespace detail {
class kqueue_impl;
} // namespace detail

/// kqueue-backed wand for BSD runtime wishes.
///
/// Each awaited wish becomes one hub-stored execution record. Kevent `udata`
/// points at that record while variant phases make prepared, parked, settled,
/// delayed-delete, and retired states explicit. The implementation lives in
/// kqueue.cpp: the wand interface is already type-erased, so none of it needs
/// to be compiled into every user of this header.
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
    void poll(deck & d);
    void wait(deck & d);

    /// Pump the deck and the kqueue until `root` has finished.
    void run_until_done(deck & d, std::coroutine_handle<> root);

    template<typename T>
    void run_until_done(deck & d, task<T> & root)
    {
        run_until_done(d, std::coroutine_handle<>{root.handle()});
    }

    void complete(deck & d, wait_token token, int result);
    void fulfill(deck & d, wait_token token);

protected:
    wait_token prep(
        deck & d,
        detail::promise_base & promise,
        detail::prepared_wish packet) override;

private:
    std::unique_ptr<detail::kqueue_impl> impl_;
};

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
