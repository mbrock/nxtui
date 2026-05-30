#pragma once

#include "nxtrt/arch.hpp"
#include "nxtrt/bell.hpp"
#include "nxtrt/task.hpp"
#include "nxtrt/wire.hpp"

#include <nxtui/input.hpp>
#include <nxtui/units.hpp>


#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>

namespace nxtrt {

namespace detail {

template<typename Fn>
[[nodiscard]] task<stored_task_result_t<Fn>> run_in_root_firm(Fn fn)
{
    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        co_await with_firm(std::move(fn));
    } else {
        co_return co_await with_firm(std::move(fn));
    }
}

} // namespace detail

/// Small application-facing owner for the runtime.
///
/// This is intentionally not the terminal UI runtime yet. It is the common
/// owner the UI runtime can be built around: one `deck`, one platform `wand`,
/// a root-firm run entrypoint, and the app-level coordination primitives that
/// the old `UIRuntime` currently gets from libcoro.
#if NXTRT_ARCH_HAS_WAND
class runtime
{
public:
    using term_size = nxtui::Size;
    using input_event = nxtui::input::KeyEvent;

    runtime()
        : deck_(&wand_)
        , resize_wire_(resize_wire_storage_)
        , input_wire_(input_wire_storage_)
    {
        debug::install_signal_dump();
    }

    runtime(const runtime &) = delete;
    runtime & operator=(const runtime &) = delete;
    runtime(runtime &&) = delete;
    runtime & operator=(runtime &&) = delete;

    [[nodiscard]] deck & current_deck() noexcept
    {
        return deck_;
    }

    [[nodiscard]] arch::wand & current_wand() noexcept
    {
        return wand_;
    }

    [[nodiscard]] bell & damage_bell() noexcept
    {
        return damage_bell_;
    }

    void signal_damage()
    {
        damage_bell_.ring();
    }

    [[nodiscard]] wire<term_size> & resize_wire() noexcept
    {
        return resize_wire_;
    }

    [[nodiscard]] wire<input_event> & input_wire() noexcept
    {
        return input_wire_;
    }

    [[nodiscard]] task<std::optional<input_event>> next_input()
    {
        co_return co_await input_wire_.next();
    }

    [[nodiscard]] task<bool> publish_input_event(input_event event)
    {
        auto published = co_await input_wire_.send(std::move(event));
        if (published)
            signal_damage();
        co_return published;
    }

    [[nodiscard]] bool publish_resize(term_size size)
    {
        auto published = resize_wire_.try_send(size);
        if (published)
            signal_damage();
        return published;
    }

    [[nodiscard]] std::optional<term_size> next_resize_now()
    {
        return resize_wire_.try_next();
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
            detail::run_in_root_firm(
                std::decay_t<Fn>{std::forward<Fn>(fn)}));
    }

    void request_stop()
    {
        stopping_ = true;
        input_wire_.close();
        resize_wire_.close();
        signal_damage();
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stopping_;
    }

private:
    arch::wand wand_;
    deck deck_;
    bell damage_bell_;
    rack<term_size> resize_wire_storage_{64};
    rack<input_event> input_wire_storage_{64};
    wire<term_size> resize_wire_;
    wire<input_event> input_wire_;
    bool stopping_ = false;
};
#endif

} // namespace nxtrt
