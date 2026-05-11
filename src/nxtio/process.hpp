#pragma once

#include "nxt/any_layout.hpp"
#include "nxt/signal.hpp"
#include "nxt/slot.hpp"
#include "nxt/tui.hpp"
#include "nxtio/app.hpp"
#include "nxtio/async-core.hpp"
#include "nxtio/input.hpp"
#include "nxtio/scope.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
        publish(OutputMessage{
            .kind = OutputMessage::Kind::print,
            .text = std::string{text},
        });
    }

    void println(std::string_view line) const
    {
        publish(OutputMessage{
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
        .publish = [&runtime](OutputMessage message) {
            if (message.kind == OutputMessage::Kind::line)
                runtime.println(message.text);
            else
                runtime.print(message.text);
        },
    };
}

/// Capability proxy passed to a process body. Everything a body needs to
/// interact with the world — drawing, spawning children, sleeping,
/// reading input, requesting shutdown — goes through `self`. The
/// underlying runtime is reachable via `runtime()` as an escape hatch
/// for things not yet wrapped here.
class Self
{
public:
    explicit Self(nxt::scope<ProcessContext> & sc)
        : scope_(&sc)
    {}

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

    // ---- Signals -------------------------------------------------------

    /// Create a new Signal<T>. The Signal is move-only and is intended
    /// to live as a local in the caller. Hand out write-endpoints via
    /// `signal.publisher()` or `signal.publisher(value)`.
    template<typename T>
    [[nodiscard]] nxt::Signal<T> signal() const
    {
        return nxt::Signal<T>{};
    }

    /// Race the next values from several Signals. The losing waits are
    /// cancelled when one signal wins, or when this process is cancelled.
    template<typename... T>
    nxt::task<std::variant<std::optional<T>...>>
    select(const nxt::Signal<T> &... signals) const
    {
        auto select_scope = scope_->subscope();
        co_return co_await select_scope.any(
            signals.next(select_scope.stop_token())...);
    }

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
    {}

    nxt::scope<ProcessContext> & scope() noexcept { return scope_; }
    const tui::Slot<tui::AnyLayout> & surface() const noexcept { return surface_; }
    Self & self() noexcept { return self_; }

private:
    tui::Slot<tui::AnyLayout> surface_;
    nxt::scope<ProcessContext> scope_;
    Self self_;
};

/// Parent-side handle to a spawned process. Cancels the child's scope
/// on destruction so child lifetimes are bounded by their handles.
class ProcessHandle
{
public:
    explicit ProcessHandle(std::shared_ptr<ProcessState> state) noexcept
        : state_(std::move(state))
    {}

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
        throw;
    }
    state->scope().cancel();
}

} // namespace detail

/// Spawn a child process within `parent`'s cancellation scope.
template<typename Body>
[[nodiscard]] ProcessHandle spawn(const Self & parent, Body body)
{
    auto state = std::make_shared<ProcessState>(
        parent.runtime(),
        parent.scope().stop_token(),
        parent.scope().context().output);

    parent.runtime().scheduler().spawn_detached(
        detail::run_body(state, std::move(body)));

    return ProcessHandle{state};
}

template<typename Body>
[[nodiscard]] ProcessHandle Self::spawn(Body body) const
{
    return nxt::ui::spawn(*this, std::move(body));
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

        auto root_runner =
            [&runtime, root_state, body = std::move(body)]()
            -> nxt::task<> {
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
