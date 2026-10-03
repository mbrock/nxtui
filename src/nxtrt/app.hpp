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

/// Small application-facing owner for the runtime.
///
/// This is intentionally not the terminal UI runtime yet. It is the common
/// owner the UI runtime can be built around: one `deck`, one platform `wand`,
/// a root run entrypoint, and the app-level coordination primitives that
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

    /// Rejects an already-created task at compile time; see
    /// `deck::sync_wait(task<T>)`.
    template<typename T>
    void run(task<T>)
    {
        static_assert(
            sizeof(T *) == 0,
            "runtime::run takes a task factory, not a task: write "
            "rt.run(make_task) or rt.run([&] { return make_task(args); }) "
            "so the root task is created inside its runtime");
    }

    /// Create the root task by calling `fn(args...)` inside the runtime,
    /// then drive it to completion.
    template<typename Fn, typename... Args>
        requires task_factory<std::decay_t<Fn> &, Args...>
    [[nodiscard]] task_result_t<
        std::invoke_result_t<std::decay_t<Fn> &, Args...>>
    run(Fn && fn, Args &&... args)
    {
        auto root_env = runtime_env{};
        auto root_guard = detail::env_guard{root_env, &deck_, nullptr};

        // The factory outlives its task: a capturing coroutine lambda's
        // frame refers to the closure object.
        auto factory = std::decay_t<Fn>{std::forward<Fn>(fn)};
        return drive(std::invoke(factory, std::forward<Args>(args)...));
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
    template<typename T>
    [[nodiscard]] T drive(task<T> root)
    {
        deck_.start(root);
        wand_.run_until_done(deck_, root);

        if constexpr (std::is_void_v<T>) {
            std::move(root).result();
        } else {
            return std::move(root).result();
        }
    }

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
