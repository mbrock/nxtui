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
};

struct ProcessContext
{
    UIRuntime * runtime;
    tui::Slot<tui::AnyLayout> surface;
    OutputPublisher output;
};

inline OutputPublisher runtime_output(UIRuntime & runtime)
{
    return OutputPublisher{
        .publish =
            [&runtime](OutputMessage message) {
                if (message.kind == OutputMessage::Kind::line)
                    runtime.println(message.text);
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
        OutputPublisher output)
        : surface_(tui::AnyLayout{}, [&rt] { rt.signal_damage(); })
        , scope_(
              rt.scheduler(),
              parent_token,
              ProcessContext{
                  .runtime = &rt,
                  .surface = surface_,
                  .output = std::move(output),
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
    try {
        co_await body(state->self());
    } catch (const nxt::cancelled &) {
        // Normal scope teardown.
    } catch (...) {
        state->scope().cancel();
        state->complete();
        throw;
    }
    state->scope().cancel();
    state->complete();
}

inline nxt::task<> join_body(std::shared_ptr<ProcessState> state)
{
    co_await state->join();
}

} // namespace detail

/// Spawn a child process within `parent`'s cancellation scope.
template<typename Body>
[[nodiscard]] ProcessHandle spawn(const yard & parent, Body body)
{
    auto state = std::make_shared<ProcessState>(
        parent.runtime(),
        parent.scope().stop_token(),
        parent.scope().context().output);

    parent.runtime().scheduler().spawn_detached(
        detail::run_body(state, std::move(body)));

    parent.scope().spawn(detail::join_body(state));

    return ProcessHandle{state};
}

template<typename Body>
[[nodiscard]] ProcessHandle yard::spawn(Body body) const
{
    return nxt::ui::spawn(*this, std::move(body));
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

        auto root_state = std::make_shared<ProcessState>(
            runtime, std::stop_token{}, runtime_output(runtime));

        tasks.push_back(runtime.signal_loop());
        tasks.push_back(runtime.input_loop());
        tasks.push_back(runtime.run_render_loop(
            [surface = root_state->surface()] { return surface; }));

        auto root_runner = [&runtime,
                            root_state,
                            body =
                                std::move(body)]() mutable -> nxt::task<> {
            try {
                co_await body(root_state->self());
            } catch (const nxt::cancelled &) {
                runtime.request_shutdown();
                co_return;
            } catch (...) {
                runtime.request_shutdown();
                throw;
            }
            runtime.request_shutdown();
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
