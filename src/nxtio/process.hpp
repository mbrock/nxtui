#pragma once

#include "nxt/ansi.hpp"
#include "nxt/any_layout.hpp"
#include "nxt/raster.hpp"
#include "nxt/slot.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"
#include "nxtio/app.hpp"
#include "nxtio/async.hpp"
#include "nxtio/input.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ui {

class ProcessState;
class ProcessHandle;

struct OutputMessage
{
    enum class Kind {
        print,
        line,
        block,
    };

    Kind kind = Kind::print;
    std::string text;
};

struct OutputPublisher
{
    std::function<void(OutputMessage)> publish;

    void print(std::string_view text) const
    {
        publish(
            OutputMessage{
                .kind = OutputMessage::Kind::print,
                .text = std::string{text},
            });
    }

    void println(std::string_view line) const
    {
        publish(
            OutputMessage{
                .kind = OutputMessage::Kind::line,
                .text = std::string{line},
            });
    }

    void print_block(std::string_view text) const
    {
        publish(
            OutputMessage{
                .kind = OutputMessage::Kind::block,
                .text = std::string{text},
            });
    }
};

class span_guard;

struct ProcessContext
{
    UIRuntime * runtime;
    tui::Slot<tui::AnyLayout> surface;
    OutputPublisher output;
    // Span attribution travels with the scope: every row emitted from
    // inside this process can pull its `span_id` from here. Inherited
    // by value when spawning children — `nxt::ui::spawn` overwrites
    // `parent_span_id` / `span_id` for the child before its body runs.
    std::string span_id;
    std::string parent_span_id;
    std::string span_name;
};

inline OutputPublisher runtime_output(UIRuntime & runtime)
{
    return OutputPublisher{
        .publish =
            [&runtime](OutputMessage message) {
                if (message.kind == OutputMessage::Kind::line)
                    runtime.println(message.text);
                else if (message.kind == OutputMessage::Kind::block)
                    runtime.print_block(message.text);
                else
                    runtime.print(message.text);
            },
    };
}

/// A yard is a drawable process scope context, or something.
class yard
{
public:
    explicit yard(nxt::scope<ProcessContext> & sc)
        : scope_(&sc)
    {
    }

    // ---- Drawing -------------------------------------------------------

    /// Publish a new layout into this process's surface. The slot wakes
    /// the render loop automatically.
    template<typename L>
        requires tui::Layout<std::decay_t<L>>
    void draw(L && layout) const
    {
        scope_->context().surface.publish(
            tui::AnyLayout(std::forward<L>(layout)));
    }

    /// The surface, suitable for embedding in a parent's layout. Cheap
    /// to copy (shared_ptr bump).
    const tui::Slot<tui::AnyLayout> & surface() const noexcept
    {
        return scope_->context().surface;
    }

    // ---- Spawning ------------------------------------------------------

    /// Spawn a child process bound to this scope. See free `spawn(self,
    /// body)` for the implementation; this is the canonical entry point.
    template<typename Body>
    [[nodiscard]] ProcessHandle spawn(Body body) const;

    /// Spawn a child process with an explicit span name. The name shows
    /// up on every trace row originating inside the child and on its
    /// `span_begin` / `span_end` markers.
    template<typename Body>
    [[nodiscard]] ProcessHandle spawn(std::string name, Body body) const;

    /// Open an RAII trace span around a region of code without
    /// introducing a new coroutine. Returns a guard that emits
    /// `span_end` on destruction.
    [[nodiscard]] span_guard span(std::string name) const;

    // ---- Time ----------------------------------------------------------

    /// Suspend this coroutine for `duration`.
    template<typename Rep, typename Period>
    nxt::task<> sleep(std::chrono::duration<Rep, Period> duration) const
    {
        co_await scope_->scheduler().yield_for(duration);
    }

    // ---- Input ---------------------------------------------------------

    /// Await the next keyboard event from the global input queue.
    /// Returns nullopt if the input stream has been shut down.
    nxt::task<std::optional<nxt::input::KeyEvent>> next_input() const
    {
        return runtime().next_input();
    }

    // ---- Output (scrollback) ------------------------------------------

    /// Print a line to the scroll region above the HUD.
    void println(std::string_view line) const
    {
        scope_->context().output.println(line);
    }

    /// Print without a trailing newline.
    void print(std::string_view text) const
    {
        scope_->context().output.print(text);
    }

    /// Print a complete block to the scrollback as one coherent output.
    void print_block(std::string_view text) const
    {
        scope_->context().output.print_block(text);
    }

    template<typename L>
        requires tui::Layout<std::decay_t<L>>
    void print(L && layout) const
    {
        auto height = layout.height_hint().min;
        if (height.count() == 0)
            height = 1 * ln;

        Raster raster(
            runtime().terminal_width(), height, runtime().glyphs());
        auto view = raster.view();
        layout.render(view, raster.extent());
        print(ansi::render_raster(raster));
    }

    void print(tui::Span span) const
    {
        print(tui::styled_text(std::move(span)));
    }

    /// Replace this process's output publisher. Child processes inherit
    /// the replacement.
    void set_output(OutputPublisher output) const
    {
        scope_->context().output = std::move(output);
    }

    /// Snapshot the current back buffer to `img/<short-id>.png` and
    /// print the path to scrollback. Returns the saved path. Useful as
    /// a "trophy" capture at interesting moments in a flow.
    std::string snapshot() const
    {
        return runtime().snapshot();
    }

    // ---- Lifecycle ----------------------------------------------------

    /// True once this process has been asked to stop. Bodies should
    /// check this at suspension points or in their loop guards.
    bool cancelled() const noexcept
    {
        return scope_->cancelled();
    }

    /// Cancel this process's scope. Child processes inherit and observe.
    void cancel() const noexcept
    {
        scope_->cancel();
    }

    /// Request the whole application to shut down.
    void request_shutdown() const noexcept
    {
        runtime().request_shutdown();
    }

    /// Explicitly wake the render loop. Rarely needed — `draw()` already
    /// does this — but useful when state outside a slot changed.
    void signal_damage() const noexcept
    {
        runtime().signal_damage();
    }

    // ---- Escape hatches -----------------------------------------------

    /// Cancellation scope for this process.
    nxt::scope<ProcessContext> & scope() const noexcept
    {
        return *scope_;
    }

    /// Direct runtime access. Prefer the wrapped methods above; this is
    /// for features that haven't been promoted onto `Self` yet.
    UIRuntime & runtime() const noexcept
    {
        return *scope_->context().runtime;
    }

private:
    nxt::scope<ProcessContext> * scope_;
};

[[nodiscard]] inline bool is_escape(const nxt::input::KeyEvent & event)
{
    return event.key == nxt::input::Key::escape;
}

[[nodiscard]] inline bool
is_character(const nxt::input::KeyEvent & event, char ch)
{
    return event.key == nxt::input::Key::character
           && event.codepoint == static_cast<std::uint32_t>(ch);
}

[[nodiscard]] inline bool is_quit_key(const nxt::input::KeyEvent & event)
{
    return is_escape(event) || is_character(event, 'q');
}

template<typename Predicate>
[[nodiscard]] nxt::task<std::optional<nxt::input::KeyEvent>>
next_key_press(const yard & self, Predicate predicate)
{
    while (!self.cancelled()) {
        auto event = co_await self.next_input();
        if (!event)
            co_return std::nullopt;
        if (event->type == nxt::input::EventType::release)
            continue;
        if (predicate(*event))
            co_return event;
    }
    co_return std::nullopt;
}

[[nodiscard]] inline nxt::task<std::optional<nxt::input::KeyEvent>>
next_key_press(const yard & self)
{
    co_return co_await next_key_press(
        self, [](const nxt::input::KeyEvent &) { return true; });
}

/// Internal state of a process. Heap-allocated and shared between the
/// running body coroutine and any ProcessHandles held by parents.
class ProcessState
{
public:
    ProcessState(
        UIRuntime & rt,
        std::stop_token parent_token,
        OutputPublisher output,
        std::string span_id,
        std::string parent_span_id,
        std::string span_name)
        : surface_(tui::AnyLayout{}, [&rt] { rt.signal_damage(); })
        , scope_(
              rt.scheduler(),
              parent_token,
              ProcessContext{
                  .runtime = &rt,
                  .surface = surface_,
                  .output = std::move(output),
                  .span_id = std::move(span_id),
                  .parent_span_id = std::move(parent_span_id),
                  .span_name = std::move(span_name),
              })
        , self_(scope_)
    {
    }

    nxt::scope<ProcessContext> & scope() noexcept
    {
        return scope_;
    }

    const tui::Slot<tui::AnyLayout> & surface() const noexcept
    {
        return surface_;
    }

    yard & self() noexcept
    {
        return self_;
    }

    void complete() noexcept
    {
        done_.count_down();
    }

    nxt::task<> join() const
    {
        co_await done_;
    }

private:
    tui::Slot<tui::AnyLayout> surface_;
    nxt::scope<ProcessContext> scope_;
    yard self_;
    nxt::latch done_{1};
};

/// Parent-side handle to a spawned process. Cancels the child's scope
/// on destruction so child lifetimes are bounded by their handles.
class ProcessHandle
{
public:
    explicit ProcessHandle(std::shared_ptr<ProcessState> state) noexcept
        : state_(std::move(state))
    {
    }

    ProcessHandle(const ProcessHandle &) = delete;
    ProcessHandle & operator=(const ProcessHandle &) = delete;

    ProcessHandle(ProcessHandle && other) noexcept = default;

    ProcessHandle & operator=(ProcessHandle && other) noexcept
    {
        if (this != &other) {
            cancel();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~ProcessHandle()
    {
        cancel();
    }

    /// Embeddable surface — copy by value into the parent's layout tree.
    const tui::Slot<tui::AnyLayout> & surface() const noexcept
    {
        return state_->surface();
    }

    /// True once the underlying scope has been cancelled.
    bool cancelled() const noexcept
    {
        return !state_ || state_->scope().cancelled();
    }

    /// Request the child to stop. Idempotent.
    void cancel() noexcept
    {
        if (state_)
            state_->scope().cancel();
    }

    /// Wait until the child body has exited.
    nxt::task<> join() const
    {
        auto state = state_;
        if (state)
            co_await state->join();
    }

private:
    std::shared_ptr<ProcessState> state_;
};

namespace detail {

template<typename Body>
nxt::task<> run_body(std::shared_ptr<ProcessState> state, Body body)
{
    auto & ctx = state->scope().context();
    auto & rt = *ctx.runtime;
    rt.emit_span_begin(ctx.span_id, ctx.parent_span_id, ctx.span_name);

    std::string_view status = "ok";
    try {
        co_await body(state->self());
    } catch (const nxt::cancelled &) {
        status = "cancelled";
    } catch (...) {
        rt.emit_span_end(
            ctx.span_id, ctx.parent_span_id, ctx.span_name, "error");
        state->scope().cancel();
        state->complete();
        throw;
    }
    rt.emit_span_end(ctx.span_id, ctx.parent_span_id, ctx.span_name, status);
    state->scope().cancel();
    state->complete();
}

inline nxt::task<> join_body(std::shared_ptr<ProcessState> state)
{
    co_await state->join();
}

} // namespace detail

/// Spawn a child process within `parent`'s cancellation scope. The
/// optional `name` becomes the child's `span_name` and is emitted on
/// every trace row that originates inside the child.
template<typename Body>
[[nodiscard]] ProcessHandle
spawn(const yard & parent, std::string name, Body body)
{
    auto & rt = parent.runtime();
    auto & parent_ctx = parent.scope().context();
    auto child_span = rt.allocate_span_id();
    auto state = std::make_shared<ProcessState>(
        rt,
        parent.scope().stop_token(),
        parent_ctx.output,
        std::move(child_span),
        parent_ctx.span_id,
        std::move(name));

    rt.scheduler().spawn_detached(
        detail::run_body(state, std::move(body)));

    parent.scope().spawn(detail::join_body(state));

    return ProcessHandle{state};
}

/// Backwards-compatible `spawn` without an explicit name.
template<typename Body>
[[nodiscard]] ProcessHandle spawn(const yard & parent, Body body)
{
    return spawn(parent, std::string{}, std::move(body));
}

template<typename Body>
[[nodiscard]] ProcessHandle yard::spawn(Body body) const
{
    return nxt::ui::spawn(*this, std::string{}, std::move(body));
}

template<typename Body>
[[nodiscard]] ProcessHandle
yard::spawn(std::string name, Body body) const
{
    return nxt::ui::spawn(*this, std::move(name), std::move(body));
}

/// RAII guard that brackets a region of work with `span_begin` /
/// `span_end` rows in the trace. Useful for naming phases inside a
/// single process body without introducing a new coroutine.
class [[nodiscard]] span_guard
{
public:
    span_guard(const yard & y, std::string name)
        : runtime_(&y.runtime())
        , parent_span_id_(y.scope().context().span_id)
        , span_id_(runtime_->allocate_span_id())
        , name_(std::move(name))
    {
        runtime_->emit_span_begin(span_id_, parent_span_id_, name_);
    }

    span_guard(const span_guard &) = delete;
    span_guard & operator=(const span_guard &) = delete;
    span_guard(span_guard &&) = delete;
    span_guard & operator=(span_guard &&) = delete;

    ~span_guard()
    {
        runtime_->emit_span_end(span_id_, parent_span_id_, name_, "ok");
    }

    [[nodiscard]] std::string_view span_id() const noexcept
    {
        return span_id_;
    }

private:
    UIRuntime * runtime_;
    std::string parent_span_id_;
    std::string span_id_;
    std::string name_;
};

inline span_guard yard::span(std::string name) const
{
    return span_guard{*this, std::move(name)};
}

inline auto spinner(
    std::chrono::milliseconds interval = std::chrono::milliseconds{80},
    tui::Style style = tui::bold | tui::fg(Rgba8::black())
                       | tui::bg(Rgba8::white()))
{
    return [=](yard & self) -> nxt::task<> {
        for (std::size_t tick = 0; !self.cancelled(); ++tick) {
            self.draw(tui::spinner(tick, style));
            co_await self.sleep(interval);
        }
    };
}

template<typename WorkerBody, typename CompanionBody, typename Layout>
nxt::task<> accompany(
    const yard & self,
    WorkerBody worker_body,
    CompanionBody companion_body,
    Layout layout)
{
    auto companion = self.spawn(std::move(companion_body));
    auto worker = self.spawn(std::move(worker_body));

    self.draw(layout(companion.surface(), worker.surface()));

    co_await worker.join();
    companion.cancel();
    co_await self.scope().all();
}

template<typename WorkerBody>
nxt::task<> spintag(const yard & self, WorkerBody worker_body)
{
    return accompany(
        self,
        worker_body,
        nxt::ui::spinner(),
        [](const auto & spinner, const auto & worker) {
            return nxt::tui::row(spinner, nxt::tui::text(" "), worker);
        });
}

/// Run a TUI application Proact-style: the body coroutine owns the
/// whole UI lifecycle. Its surface is mounted as the screen root, and
/// the program exits when the body returns.
template<typename Body>
int run2(Body body)
{
    UIRuntime runtime;
    std::vector<nxt::task<>> tasks;

    try {
        TerminalGuard guard;
        nxt::input::InputModeGuard input_guard;

        // Wire the root yard's context to the runtime's root span so
        // every `self.spawn(...)` inside `body` becomes a child of
        // `runtime` in the trace. The root yard itself shares the
        // runtime span — `run2` doesn't emit its own span_begin/end
        // because UIRuntime already brackets the run.
        auto root_state = std::make_shared<ProcessState>(
            runtime,
            std::stop_token{},
            runtime_output(runtime),
            std::string{runtime.root_span_id()},
            std::string{},
            "runtime");

        tasks.push_back(runtime.signal_loop());
        tasks.push_back(runtime.input_loop());
        tasks.push_back(runtime.run_render_loop(
            [surface = root_state->surface()] { return surface; }));

        auto root_runner = [&runtime,
                            root_state,
                            body =
                                std::move(body)]() mutable -> nxt::task<> {
            // Capture any body exception outside the catch so the
            // drain step below (which co_awaits) can run before
            // propagation. co_await inside a catch handler is
            // disallowed by the language, so we use this trampoline.
            std::exception_ptr eptr;
            try {
                co_await body(root_state->self());
            } catch (const nxt::cancelled &) {
                // Treat cancellation as a normal exit; we still want
                // children to drain so their span_end rows land in
                // the trace.
            } catch (...) {
                eptr = std::current_exception();
            }
            // Cancel any still-running children so they exit promptly,
            // then await every `join_body` task spawned into the
            // root scope. Without this, detached child coroutines may
            // be torn down by the scheduler before `run_body` reaches
            // the line that emits their `span_end`.
            root_state->scope().cancel();
            try {
                co_await root_state->scope().all();
            } catch (const nxt::cancelled &) {
            } catch (...) {
                // Suppress: we're already winding down. The body's
                // exception (if any) is the one that matters.
            }
            runtime.request_shutdown();
            if (eptr)
                std::rethrow_exception(eptr);
        };
        tasks.push_back(root_runner());

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
