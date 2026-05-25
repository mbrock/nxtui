#pragma once

#include "nxt/rt/channel.hpp"
#include "nxt/rt/event.hpp"
#include "nxt/rt/task.hpp"

#include <nxt/units.hpp>
#include <nxtio/input.hpp>

#if defined(__linux__)
#include "nxt/rt/uring_wand.hpp"
#else
#include "nxt/rt/kqueue_wand.hpp"
#endif

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>

namespace nxt::rt {

#if defined(__linux__) && NXT_RT_HAS_LIBURING
using platform_wand = uring_wand;
inline constexpr bool has_platform_wand = true;
#elif !defined(__linux__) && NXT_RT_HAS_KQUEUE
using platform_wand = kqueue_wand;
inline constexpr bool has_platform_wand = true;
#else
inline constexpr bool has_platform_wand = false;
#endif

namespace detail {

template<typename Fn>
[[nodiscard]] task<stored_task_result_t<Fn>> run_in_root_zone(Fn fn)
{
    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        co_await with_zone(std::move(fn));
    } else {
        co_return co_await with_zone(std::move(fn));
    }
}

} // namespace detail

/// Small application-facing owner for the ng runtime.
///
/// This is intentionally not the terminal UI runtime yet. It is the common
/// owner the UI runtime can be built around: one `deck`, one platform `wand`,
/// a root-zone run entrypoint, and the app-level coordination primitives that
/// the old `UIRuntime` currently gets from libcoro.
#if (defined(__linux__) && NXT_RT_HAS_LIBURING) \
    || (!defined(__linux__) && NXT_RT_HAS_KQUEUE)
class runtime
{
public:
    using term_size = nxt::Size;
    using input_event = nxt::input::KeyEvent;

    runtime()
        : deck_(&wand_)
    {}

    runtime(const runtime &) = delete;
    runtime & operator=(const runtime &) = delete;
    runtime(runtime &&) = delete;
    runtime & operator=(runtime &&) = delete;

    [[nodiscard]] deck & current_deck() noexcept
    {
        return deck_;
    }

    [[nodiscard]] platform_wand & current_wand() noexcept
    {
        return wand_;
    }

    [[nodiscard]] event & damage_event() noexcept
    {
        return damage_event_;
    }

    void signal_damage()
    {
        damage_event_.set();
    }

    [[nodiscard]] channel<term_size> & resize_channel() noexcept
    {
        return resize_channel_;
    }

    [[nodiscard]] channel<input_event> & input_channel() noexcept
    {
        return input_channel_;
    }

    [[nodiscard]] task<std::optional<input_event>> next_input()
    {
        co_return co_await input_channel_.next();
    }

    [[nodiscard]] task<bool> publish_input_event(input_event event)
    {
        auto published = co_await input_channel_.publish(std::move(event));
        if (published)
            signal_damage();
        co_return published;
    }

    [[nodiscard]] bool publish_resize(term_size size)
    {
        auto published = resize_channel_.try_publish(size);
        if (published)
            signal_damage();
        return published;
    }

    [[nodiscard]] std::optional<term_size> next_resize_now()
    {
        return resize_channel_.try_pop();
    }

    template<typename Rep, typename Period>
    [[nodiscard]] task<void>
    sleep(std::chrono::duration<Rep, Period> duration)
    {
        co_await op::timeout::after(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                duration));
    }

    template<typename T>
    [[nodiscard]] T run(task<T> root)
    {
        deck_.start(root);
        wand_.run_until_done(deck_, root);

        if constexpr (std::is_void_v<T>) {
            std::move(root).result();
        } else {
            return std::move(root).result();
        }
    }

    template<typename Fn>
        requires stored_task_factory<std::decay_t<Fn>>
    [[nodiscard]] stored_task_result_t<std::decay_t<Fn>>
    run(Fn && fn)
    {
        return run(
            detail::run_in_root_zone(
                std::decay_t<Fn>{std::forward<Fn>(fn)}));
    }

    void request_stop()
    {
        input_channel_.cancel();
        resize_channel_.cancel();
        signal_damage();
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return input_channel_.stop_requested()
            || resize_channel_.stop_requested();
    }

private:
    platform_wand wand_;
    deck deck_;
    event damage_event_;
    channel<term_size> resize_channel_;
    channel<input_event> input_channel_;
};
#endif

} // namespace nxt::rt
