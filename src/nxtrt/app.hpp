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

/// Application owner of the runtime: one deck, the platform wand, and a run
/// entry.
///
/// A `runtime` owns an `arch::wand` (the build's default wand) and a `deck`
/// driven by it, plus a few application coordination channels: a damage
/// `bell`, a bounded `wire` of terminal sizes, and a bounded `wire` of key
/// events (64 slots each). Call `run` with a task factory; the root and
/// every task it starts run on the thread that called `run`. The
/// constructor installs the SIGUSR1 runtime dump (see
/// `debug::install_signal_dump`). Not copyable or movable. Only defined
/// when the platform has a default wand (`NXTRT_ARCH_HAS_WAND`).
///
/// @code
/// auto rt = nxtrt::runtime{};
/// rt.run([&]() -> nxtrt::task<void> {
///     co_await rt.sleep(std::chrono::milliseconds{10});
/// });
/// @endcode
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

    /// Bell rung whenever the application should redraw: by
    /// `signal_damage`, by published input or resize, and by
    /// `request_stop`. It is manual-reset; see @ref nxtrt::bell "bell".
    [[nodiscard]] bell & damage_bell() noexcept
    {
        return damage_bell_;
    }

    /// Rings the damage bell.
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

    /// Awaits the next key event; `std::nullopt` once the input wire is
    /// closed (by `request_stop`) and drained.
    [[nodiscard]] task<std::optional<input_event>> next_input()
    {
        co_return co_await input_wire_.next();
    }

    /// Sends a key event, waiting while the input wire is full, and rings
    /// the damage bell. Returns false if the wire is closed.
    [[nodiscard]] task<bool> publish_input_event(input_event event)
    {
        auto published = co_await input_wire_.send(std::move(event));
        if (published)
            signal_damage();
        co_return published;
    }

    /// Queues a terminal size without waiting and rings the damage bell.
    /// Returns false, dropping `size`, if the resize wire is full or
    /// closed.
    [[nodiscard]] bool publish_resize(term_size size)
    {
        auto published = resize_wire_.try_send(size);
        if (published)
            signal_damage();
        return published;
    }

    /// Takes the oldest queued terminal size, if any, without waiting.
    [[nodiscard]] std::optional<term_size> next_resize_now()
    {
        return resize_wire_.try_next();
    }

    /// Waits `duration` with a `timeout` wish on the running deck's wand.
    /// Throws `operation_cancelled` if the task is stopped first.
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
    ///
    /// Blocks the calling thread in the wand's `run_until_done` until the
    /// root finishes, then returns its result or rethrows its exception.
    /// The factory is moved into storage that outlives the root task, so a
    /// capturing coroutine lambda is safe. Not reentrant.
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

    /// Marks the runtime as stopping, closes the input and resize wires,
    /// and rings the damage bell.
    ///
    /// It does not stop the root task; application loops should check
    /// `stop_requested()` or end when `next_input()` yields `std::nullopt`.
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
