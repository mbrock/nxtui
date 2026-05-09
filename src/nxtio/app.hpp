#pragma once

#include <coro/when_any.hpp>
#include <stop_token>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "nxt/ansi.hpp"
#include "nxtio/async.hpp"
#include "nxt/compositor.hpp"
#include "nxt/glyph-table.hpp"
#include "nxtio/input.hpp"
#include "nxt/raster.hpp"
#include "nxtio/signal-pipe.hpp"
#include "nxt/units.hpp"

namespace nxt::ui {

using TermSize = nxt::Size;

// Re-export TerminalGuard for convenience
using ansi::TerminalGuard;

/// Runtime state for the UI system.
/// Owns scheduler, glyph table, compositor, and coordinates
/// signals/events.
class UIRuntime
{
public:
    UIRuntime();
    ~UIRuntime();

    // Non-copyable, non-moveable (owns resources)
    UIRuntime(const UIRuntime &) = delete;
    UIRuntime & operator=(const UIRuntime &) = delete;
    UIRuntime(UIRuntime &&) = delete;
    UIRuntime & operator=(UIRuntime &&) = delete;

    /// Access the scheduler.
    [[nodiscard]] nxt::io_scheduler & scheduler() noexcept
    {
        return *scheduler_;
    }

    /// Access the scheduler owner for libcoro networking APIs.
    [[nodiscard]] std::unique_ptr<nxt::io_scheduler> &
    scheduler_handle() noexcept
    {
        return scheduler_;
    }

    /// Access the glyph table.
    [[nodiscard]] GlyphTable & glyphs() noexcept
    {
        return glyphs_;
    }

    /// Check if shutdown has been requested.
    [[nodiscard]] bool shutdown_requested() const noexcept
    {
        return stop_source_.stop_requested();
    }

    /// Request shutdown.
    void request_shutdown();

    /// Run a task, then request shutdown when it completes.
    nxt::task<> shutdown_after(nxt::task<> t)
    {
        co_await t;
        request_shutdown();
    }

    template<class rep_type, class period_type>
    nxt::task<> sleep(std::chrono::duration<rep_type, period_type> duration)
    {
        co_await scheduler().yield_for(duration);
    }

    /// Signal that the view has been damaged and needs redraw.
    void signal_damage();

    /// Stop token
    std::stop_token get_stop_token() const noexcept
    {
        return stop_source_.get_token();
    }

    /// Current terminal dimensions.
    [[nodiscard]] TermSize terminal_size() const noexcept;
    [[nodiscard]] width_t terminal_width() const noexcept;
    [[nodiscard]] height_t terminal_height() const noexcept;

    template<typename... Tasks>
    auto run(Tasks &&... tasks)
    {
        for (auto & task : {std::forward<Tasks>(tasks)...}) {
            scheduler().schedule(std::move(task));
        }
        return nxt::when_all(std::forward<Tasks>(tasks)...);
    }

    // =========================================================================
    // Render loop helpers
    // =========================================================================

    /// Render a layout to the screen.
    /// Computes HUD height from layout hint, sets up scroll region,
    /// renders.
    template<typename Layout>
    void render(const Layout & layout)
    {
        ansi::SynchronizedUpdate synchronized_update;
        render_frame(layout);
    }

    /// Print a line to the scroll region (only works when HUD
    /// height < terminal). In full-screen mode, this is a no-op.
    void println(std::string_view line);

    /// Write text to the scroll region cursor. HUD rendering preserves this
    /// cursor, so long-running streams can emit incremental text while the HUD
    /// keeps redrawing below.
    void print(std::string_view text);

    /// Clean up before exit - clears HUD region.
    void cleanup();

    /// Run a render loop until shutdown.
    /// BuildUI is called each frame to produce the layout.
    /// Waits for damage signal, but rate-limits to frame_time.
    /// Note: pass by value to avoid dangling references in
    /// coroutine.
    template<typename BuildUI>
    nxt::task<> run_render_loop(
        BuildUI build_ui,
        std::chrono::milliseconds frame_time = std::chrono::milliseconds{
            16})
    {
        // Initial render
        render(build_ui());
        auto last_render = std::chrono::steady_clock::now();

        while (!shutdown_requested()) {
            // Wait for damage
            //          damage_event_.reset();
            //            auto ping =
            //            scheduler_->schedule_after(frame_time);

            if (shutdown_requested())
                break;

            // Rate limit: wait until frame_time has passed since last
            // render
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_render);
            if (elapsed < frame_time)
                co_await scheduler_->yield_for(frame_time - elapsed);

            {
                ansi::SynchronizedUpdate synchronized_update;

                std::optional<TermSize> resize_to;
                if (refresh_terminal_size()) {
                    scrollback_cursor_initialized_ = false;
                    resize_to = terminal_size();
                }

                // Process any pending resizes, keeping only the newest size.
                while (auto sz = resize_queue_.try_pop())
                    resize_to = *sz;
                if (resize_to)
                    compositor_->resize(*resize_to);

                render_frame(build_ui());
            }

            last_render = std::chrono::steady_clock::now();
        }
    }

    /// Coroutine that handles signals from the pipe.
    /// Should be run as part of the main task group.
    nxt::task<> signal_loop();

    /// Coroutine that reads stdin and publishes decoded keyboard events.
    nxt::task<> input_loop();

    /// Wait for the next keyboard event. Returns nullopt if the channel
    /// shuts down.
    nxt::task<std::optional<nxt::input::KeyEvent>> next_input()
    {
        auto event = co_await input_queue_.pop();
        if (event.has_value())
            co_return std::move(event.value());
        co_return std::nullopt;
    }

    // =========================================================================
    // Low-level access (for advanced use)
    // =========================================================================

    /// Channel for resize notifications.
    nxt::queue<TermSize> & resize_channel() noexcept
    {
        return resize_queue_;
    }

    /// Channel for keyboard input events.
    nxt::queue<nxt::input::KeyEvent> & input_channel() noexcept
    {
        return input_queue_;
    }

    /// Event signaled when damage occurs.
    nxt::event & damage_event() noexcept
    {
        return damage_event_;
    }

    /// Direct access to compositor (for testing or advanced use).
    [[nodiscard]] TerminalCompositor & compositor() noexcept;

private:
    bool refresh_terminal_size() noexcept;
    void render_impl(std::function<void(RasterView &, Size)> render_fn);
    void update_hud_height(height_t hud_h);

    template<typename Layout>
    void render_frame(const Layout & layout)
    {
        // Compute HUD height from layout
        auto hint = layout.height_hint();
        auto term_h = terminal_height();

        // If layout wants to grow, use full screen; otherwise use min
        // height
        auto wants_fullscreen = hint.flex > 0 * one;
        height_t target_h = wants_fullscreen ? term_h : hint.min;
        target_h = std::min(target_h, term_h); // Clamp to terminal

        if (!wants_fullscreen && target_h > 0 * ln) {
            auto reserved_log_rows = 7 * ln;
            auto separator = 1 * ln;
            if (term_h > reserved_log_rows + separator) {
                auto max_hud_h = term_h - reserved_log_rows - separator;
                target_h = std::min(target_h, max_hud_h);
            }
        }

        auto target_rows = static_cast<double>(
            target_h.count());

        if (wants_fullscreen || !has_smoothed_hud_height_) {
            smoothed_hud_rows_ = target_rows;
            has_smoothed_hud_height_ = true;
        } else if (target_rows > smoothed_hud_rows_) {
            smoothed_hud_rows_ = target_rows;
        } else {
            smoothed_hud_rows_ =
                hud_shrink_alpha_ * target_rows
                + (1.0 - hud_shrink_alpha_) * smoothed_hud_rows_;
        }

        auto hud_rows = static_cast<std::size_t>(
            std::round(smoothed_hud_rows_));
        hud_rows = std::min(
            hud_rows,
            static_cast<std::size_t>(term_h.count()));

        update_hud_height(hud_rows * ln);

        render_impl([&layout](RasterView & view, Size size) {
            layout.render(view, size);
        });
    }

    std::unique_ptr<nxt::io_scheduler> scheduler_;
    GlyphTable glyphs_;
    std::unique_ptr<TerminalCompositor> compositor_;
    SignalPipe signals_;

    nxt::event damage_event_;
    nxt::queue<TermSize> resize_queue_;
    nxt::queue<nxt::input::KeyEvent> input_queue_;
    nxt::poll_stop_source input_poll_stop_;

    std::atomic<nxt::width_t> term_width_{80 * ch};
    std::atomic<nxt::height_t> term_height_{24 * ln};
    std::atomic<std::uint64_t> damage_counter_{0};
    bool scrollback_cursor_initialized_{false};
    bool has_smoothed_hud_height_{false};
    double smoothed_hud_rows_{0.0};
    double hud_shrink_alpha_{0.05};

    std::stop_source stop_source_;
};

// ============================================================================
// Convenient app runner
// ============================================================================

/// Run a TUI application.
/// - initial_state: the starting state
/// - build_ui: (const State&) → Layout
/// - update: (UIRuntime&, State&) → nxt::task<> (should call
/// request_shutdown)
template<typename State, typename BuildUI, typename Update>
int run(State initial_state, BuildUI build_ui, Update update)
{
    UIRuntime runtime;
    State state = std::move(initial_state);

    std::vector<nxt::task<>> tasks;
    try {
        TerminalGuard guard;
        nxt::input::InputModeGuard input_guard;

        tasks.push_back(runtime.signal_loop());
        tasks.push_back(runtime.input_loop());
        tasks.push_back(runtime.run_render_loop(
            [&state, build_ui] { return build_ui(state); }));
        tasks.push_back(update(runtime, state));

        nxt::sync_wait(nxt::when_all(std::move(tasks)));
        runtime.cleanup();
    } catch (const std::exception & e) {
        runtime.cleanup();
        std::cerr << "Error: " << e.what() << '\n';
        std::exit(1);
    } catch (...) {
        runtime.cleanup();
        throw;
    }

    return 0;
}

} // namespace nxt::ui
