#pragma once

#include <stop_token>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include "nxt/ansi.hpp"
#include "nxtio/arrow.hpp"
#include "nxtio/async-core.hpp"
#include "nxt/compositor.hpp"
#include "nxt/glyph-table.hpp"
#include "nxtio/input.hpp"
#include "nxt/raster.hpp"
#include "nxtio/signal-pipe.hpp"
#include "nxtio/stacktrace.hpp"
#include "nxt/units.hpp"
#include "nxt/vterm.hpp"

#include <mutex>
#include <streambuf>
#include <string>
#include <vector>

namespace nxt::ui {

/// Terminal dimensions in nxt cell units.
using TermSize = nxt::Size;

/// Re-exported terminal cleanup guard for applications.
using ansi::TerminalGuard;

struct UIRuntimeOptions
{
    bool render = true;
    bool read_input = true;
};

/// Runtime state for the UI system.
/// Owns scheduler, glyph table, compositor, and coordinates
/// signals/events.
class UIRuntime
{
public:
    /// Create runtime resources and discover the initial terminal size.
    explicit UIRuntime(UIRuntimeOptions options = {});
    /// Release runtime resources.
    ~UIRuntime();

    // Non-copyable, non-moveable (owns resources)
    UIRuntime(const UIRuntime &) = delete;
    UIRuntime & operator=(const UIRuntime &) = delete;
    UIRuntime(UIRuntime &&) = delete;
    UIRuntime & operator=(UIRuntime &&) = delete;

    /// Access the scheduler.
    [[nodiscard]] nxt::scheduler & scheduler() noexcept
    {
        return *scheduler_;
    }

    /// Access the scheduler owner for libcoro networking APIs.
    [[nodiscard]] std::unique_ptr<nxt::scheduler> &
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

    /// Mark the overall runtime span status used when tracing closes.
    void mark_failed(std::string status = "error") noexcept;

    /// Run a task, then request shutdown when it completes.
    nxt::task<> shutdown_after(nxt::task<> t)
    {
        co_await t;
        request_shutdown();
    }

    /// Sleep on the runtime scheduler.
    template<class rep_type, class period_type>
    nxt::task<> sleep(std::chrono::duration<rep_type, period_type> duration)
    {
        co_await scheduler().yield_for(duration);
    }

    /// Signal that the view has been damaged and needs redraw.
    void signal_damage();

    /// Stop token tied to runtime shutdown.
    std::stop_token get_stop_token() const noexcept
    {
        return stop_source_.get_token();
    }

    /// Current terminal dimensions.
    [[nodiscard]] TermSize terminal_size() const noexcept;
    [[nodiscard]] width_t terminal_width() const noexcept;
    [[nodiscard]] height_t terminal_height() const noexcept;
    [[nodiscard]] bool has_terminal_surface() const noexcept
    {
        return terminal_surface_;
    }

    [[nodiscard]] bool render_enabled() const noexcept
    {
        return options_.render;
    }

    [[nodiscard]] bool read_input_enabled() const noexcept
    {
        return options_.read_input;
    }

    /// Schedule tasks on the runtime scheduler and await all of them.
    template<typename... Tasks>
    auto run(Tasks &&... tasks)
    {
        for (auto & task : {std::forward<Tasks>(tasks)...}) {
            scheduler().schedule(std::move(task));
        }
        return nxt::when_all(std::forward<Tasks>(tasks)...);
    }

    /// Render a layout to the screen.
    /// Computes HUD height from layout hint, sets up scroll region,
    /// renders.
    template<typename Layout>
    void render(const Layout & layout)
    {
        if (!has_terminal_surface())
            return;
        ansi::SynchronizedUpdate synchronized_update;
        render_frame(layout);
    }

    /// Queue a one-line block for the scrollback.
    void println(std::string_view line);

    /// Queue a complete block of scrollback lines. Scroller output is always
    /// atomic line blocks; arbitrary cursor-relative text is intentionally not
    /// supported here.
    void print_block(std::string_view text);

    /// Queue text to print after TerminalGuard restores the terminal.
    void print_after_exit(std::string text);

    /// Flush queued output before exit, preserving the final rendered HUD.
    void cleanup();

    /// Clear the live terminal surface before printing a crash report.
    void cleanup_for_crash();

    /// Capture the current back buffer to `img/<short-id>.png` and
    /// print the path to scrollback. Returns the saved path, or an
    /// empty string if no image was written (no PNG support, no
    /// terminal surface, or filesystem failure).
    std::string snapshot();

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
        if (!has_terminal_surface())
            co_return;

        render(build_ui());
        auto last_render = std::chrono::steady_clock::now();

        while (!shutdown_requested()) {
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
                if (refresh_terminal_size())
                    resize_to = terminal_size();

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

    /// Publish one decoded keyboard event to the input queue.
    nxt::task<> publish_input_event(nxt::input::KeyEvent event);

    /// Wait for the next keyboard event. Returns nullopt if the channel
    /// shuts down.
    nxt::task<std::optional<nxt::input::KeyEvent>> next_input()
    {
        auto event = co_await input_queue_.pop();
        if (event.has_value())
            co_return std::move(event.value());
        co_return std::nullopt;
    }

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

    // ---- Tracing -------------------------------------------------------

    /// The active trace writer for this run. Always present, but only
    /// emits rows when an output path was resolved (env `NXT_TRACE` or
    /// the default `traces/<run_id>.arrow`). Safe to call from any
    /// thread that holds the runtime alive.
    [[nodiscard]] nxt::io::arrow::ipc_trace & trace() noexcept
    {
        return trace_;
    }

    /// Stable run id used as a tag for traces, screenshots, and any
    /// other artifacts that should be associated with this process
    /// invocation.
    [[nodiscard]] std::string_view run_id() const noexcept
    {
        return run_id_;
    }

    /// Id of the implicit "root" span — used as the parent for every
    /// top-level `yard::spawn` and for non-yard producers (compositor,
    /// input pump) that emit rows outside any user span.
    [[nodiscard]] std::string_view root_span_id() const noexcept
    {
        return root_span_id_;
    }

    /// Mint a fresh span id. Each id is independently random; no
    /// coordination is needed across threads.
    [[nodiscard]] std::string allocate_span_id() const;

    /// Emit a `span_begin` row. Called by `nxt::ui::spawn` and the
    /// `yard::span` RAII helper.
    void emit_span_begin(
        std::string_view span_id,
        std::string_view parent_span_id,
        std::string_view name);

    /// Emit a `span_end` row.
    void emit_span_end(
        std::string_view span_id,
        std::string_view parent_span_id,
        std::string_view name,
        std::string_view status = {});

    /// Emit a `frame` row carrying the current visible terminal screen as a
    /// packed raster payload. Skips emission when the raster is byte-identical
    /// to the previous frame (so an idle HUD does not flood the trace).
    void record_frame_snapshot();

    /// Emit an `input` row describing one decoded key event.
    void record_input_event(const nxt::input::KeyEvent & event);

    /// Emit a `tty init` row capturing the terminal geometry at run
    /// start. Required for a replayer to size its vterm correctly.
    void record_tty_init();

    /// Emit a `tty resize` row when SIGWINCH changes the geometry.
    void record_tty_resize();

    /// Emit a `tty bytes` row carrying raw bytes written to stdout.
    /// Called from `TeeStreambuf` so the replay sees the same byte
    /// stream the real terminal saw (HUD frames + scrollback).
    void record_tty_bytes(std::string_view bytes);

    /// Mutex serializing queued scrollback blocks and writes to `std::cout`.
    /// Body coroutines enqueue output under this lock; the render loop drains
    /// it and presents the HUD while holding the same lock.
    [[nodiscard]] std::mutex & output_mutex() noexcept
    {
        return output_mutex_;
    }

private:
    struct QueuedOutput
    {
        std::string text;
    };

    bool refresh_terminal_size() noexcept;
    void render_impl(std::function<void(RasterView &, Size)> render_fn);
    void update_hud_height(height_t hud_h);
    [[nodiscard]] std::optional<row_t> query_insertion_cursor() const;
    void enqueue_output(QueuedOutput output);
    void flush_output_queue(std::ostream & out);
    void write_output(std::ostream & out, const QueuedOutput & output);

    template<typename Layout>
    void render_frame(const Layout & layout)
    {
        auto hint = layout.height_hint();
        if (!has_terminal_surface())
            return;

        auto term_h = terminal_height();

        // Non-flexing HUDs leave a small scrollback region visible when there
        // is enough terminal height; flexing layouts claim the whole terminal.
        auto wants_fullscreen = hint.flex > 0 * one;
        height_t target_h = wants_fullscreen ? term_h : hint.min;
        target_h = std::min(target_h, term_h);

        if (!wants_fullscreen && target_h > 0 * ln) {
            auto reserved_log_rows = 7 * ln;
            if (term_h > reserved_log_rows) {
                auto max_hud_h = term_h - reserved_log_rows;
                target_h = std::min(target_h, max_hud_h);
            }
        }

        // Snap HUD height to the layout's current target. A previous low-pass
        // filter made shrink transitions pleasant, but it also moved the
        // scroll-region bottom across several frames. Queued scrollback output
        // could then be emitted against an intermediate bottom row and appear
        // to gain extra blank lines as the HUD continued shrinking.
        auto hud_rows = static_cast<std::size_t>(target_h.count());
        hud_rows = std::min(
            hud_rows,
            static_cast<std::size_t>(term_h.count()));

        auto next_hud_height = hud_rows * ln;
        {
            auto guard = std::scoped_lock{output_mutex_};
            flush_output_queue(std::cout);
        }

        update_hud_height(next_hud_height);

        render_impl([&layout](RasterView & view, Size size) {
            layout.render(view, size);
        });
    }

    // Output mutex declared before compositor_ so it is constructed
    // first; the compositor is given a pointer to it during runtime
    // setup and must be able to use it across its whole lifetime.
    std::mutex output_mutex_;

    UIRuntimeOptions options_;
    std::unique_ptr<nxt::scheduler> scheduler_;
    bool terminal_surface_{false};
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
    bool scrollback_has_output_{false};
    std::vector<QueuedOutput> output_queue_;
    std::vector<std::string> post_exit_blocks_;

    // Trace stream: run id + root span id are minted up front so any
    // producer can emit a row before the first body coroutine runs.
    // Declared before screenshot_session_tag_ because the latter is
    // derived from run_id_ in the initializer list.
    std::string run_id_;
    std::string root_span_id_;
    std::string final_status_ = "ok";
    nxt::io::arrow::ipc_trace trace_;
    std::int64_t next_frame_seq_{0};
    std::uint64_t last_frame_hash_{0};
    bool has_last_frame_hash_{false};

    // Auto-screenshot (only does work when NXT_HAVE_PNG is defined).
    std::chrono::steady_clock::time_point screenshot_start_{
        std::chrono::steady_clock::now()};
    std::size_t screenshot_milestone_index_{0};
    std::string screenshot_session_tag_;
    std::vector<std::string> snapshot_paths_;
    void maybe_screenshot() noexcept;
    void capture_screenshot(std::string_view milestone) noexcept;
    Raster visible_screen_raster();

    // Mirror of everything written to stdout, fed into a headless
    // libvterm. `vt_` models the same screen the real terminal sees,
    // so snapshot() captures HUD + scrollback exactly as the user
    // does. Tee installed in the constructor for TTY runs only; cout
    // is restored in the destructor before vt_ goes away.
    std::unique_ptr<nxt::vterm::Terminal> vt_;
    std::unique_ptr<std::streambuf> tee_buf_;
    std::streambuf * saved_cout_buf_{nullptr};
    void sync_vterm_size() noexcept;

    std::stop_source stop_source_;
};

/// Print a top-level exception report for the active process boundary.
/// Returns the process exit status callers should use.
int report_exception(std::exception_ptr failure) noexcept;

} // namespace nxt::ui
