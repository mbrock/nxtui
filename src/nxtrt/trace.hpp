#pragma once

#include "nxtrt/env.hpp"
#include "nxtrt/format.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NXT_RT_ENABLE_TRACE
#define NXT_RT_ENABLE_TRACE 1
#endif

namespace nxtrt {

/// True when the `NXT_RT_TRACE` environment variable is `1`, `true`,
/// `yes`, or `on`.
inline bool trace_env_enabled() noexcept
{
#if defined(_WIN32)
    // UWP's CRT does not expose getenv. Hosts can set trace_enabled directly.
    return false;
#else
    auto const * raw = std::getenv("NXT_RT_TRACE");
    if (raw == nullptr)
        return false;

    auto value = std::string_view{raw};
    return value == "1" || value == "true" || value == "yes"
        || value == "on";
#endif
}

/// Whether `trace()` writes; read from the environment at startup and
/// assignable at run time.
inline bool trace_enabled = trace_env_enabled();

/// Writes one `[nxtrt]`-prefixed line to stderr when `trace_enabled`.
///
/// This is the runtime's low-level debug log of task, wish, and wand
/// events. Building with `NXT_RT_ENABLE_TRACE=0` compiles it out. The
/// formatting overload takes `std::format` arguments.
inline void trace(std::string_view message)
{
    if constexpr (NXT_RT_ENABLE_TRACE) {
        if (!trace_enabled)
            return;
        std::cerr << "[nxtrt] " << message << '\n';
    }
}

template<typename... Args>
inline void trace(std::format_string<Args...> fmt, Args &&... args)
{
    if constexpr (NXT_RT_ENABLE_TRACE) {
        if (!trace_enabled)
            return;
        std::cerr << "[nxtrt] "
                  << nxtrt::format(fmt, std::forward<Args>(args)...)
                  << '\n';
    }
}

/// Clock used for span and event timestamps.
using trace_clock = std::chrono::steady_clock;

/// Key/value annotation on a span or event.
struct trace_attribute
{
    std::string key;
    std::string value;
};

using trace_attributes = std::vector<trace_attribute>;

/// What a `trace_record` reports.
enum class trace_record_kind
{
    span_begin,
    span_end,
    event,
};

/// One span start, span end, or event, as delivered to observers.
struct trace_record
{
    trace_record_kind kind = trace_record_kind::event;
    std::string span_id;
    std::string parent_span_id;
    std::string name;
    std::string status;
    trace_clock::time_point at{};
    trace_clock::time_point start{};
    trace_clock::time_point end{};
    trace_attributes attributes;
};

/// Stored state of one span: identity, status, times, and attributes.
/// `end` stays default and `status` empty until the span finishes.
struct trace_span_snapshot
{
    std::string span_id;
    std::string parent_span_id;
    std::string name;
    std::string status;
    trace_clock::time_point start{};
    trace_clock::time_point end{};
    trace_attributes attributes;
};

class trace_context;

/// Handle to a span started by `trace_context::start_span`.
///
/// Copyable; copies refer to the same span and keep the context alive. A
/// default-constructed span is empty (false) and ignores `event` and
/// `finish`. Finishing is explicit: destroying a span does not end it.
class trace_span
{
public:
    trace_span() = default;

    trace_span(
        std::shared_ptr<trace_context> context,
        std::string span_id,
        std::string parent_span_id,
        std::string name) noexcept
        : context_(std::move(context))
        , span_id_(std::move(span_id))
        , parent_span_id_(std::move(parent_span_id))
        , name_(std::move(name))
    {}

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return context_ != nullptr;
    }

    [[nodiscard]] const std::string & span_id() const noexcept
    {
        return span_id_;
    }

    [[nodiscard]] const std::string & parent_span_id() const noexcept
    {
        return parent_span_id_;
    }

    [[nodiscard]] const std::string & name() const noexcept
    {
        return name_;
    }

    /// Publishes an event attributed to this span.
    void event(std::string name, trace_attributes attributes = {}) const;
    /// Ends the span with `status` (by convention `"ok"` or `"error"`).
    void finish(std::string status = "ok") const;

private:
    std::shared_ptr<trace_context> context_;
    std::string span_id_;
    std::string parent_span_id_;
    std::string name_;
};

/// In-memory collector of trace spans and events with live observers.
///
/// Must be owned by a `std::shared_ptr` (`start_span` uses
/// `shared_from_this`). It keeps a snapshot of every span it has started
/// and calls each observer synchronously for every record, outside its
/// lock. Thread-safe. Tasks find the current context and parent span
/// through the runtime environment keys `trace_context_key` and
/// `trace_current_span_key`; `with_trace_span` starts a child span around
/// a task.
///
/// @code
/// auto trace = std::make_shared<nxtrt::trace_context>();
/// auto root = trace->start_span("root");
/// co_await nxtrt::with_env<nxtrt::trace_context_key>(
///     trace, [&]() -> nxtrt::task<void> {
///         co_await nxtrt::with_env<nxtrt::trace_current_span_key>(
///             root.span_id(), [&]() -> nxtrt::task<void> {
///                 co_await nxtrt::with_trace_span("child", make_child);
///             });
///     });
/// root.finish("ok");
/// auto spans = trace->children(root.span_id()); // one span: "child"
/// @endcode
class trace_context : public std::enable_shared_from_this<trace_context>
{
public:
    using observer = std::function<void(const trace_record &)>;

    /// Starts a span under `parent_span_id` (empty for a root span) and
    /// publishes its start record. Span ids are unique per process.
    trace_span start_span(
        std::string name,
        std::string parent_span_id = {},
        trace_attributes attributes = {})
    {
        auto id = next_id();
        auto now = trace_clock::now();
        auto record = trace_record{
            .kind = trace_record_kind::span_begin,
            .span_id = id,
            .parent_span_id = parent_span_id,
            .name = name,
            .status = {},
            .at = now,
            .start = now,
            .end = {},
            .attributes = std::move(attributes),
        };

        publish(record);
        return trace_span{
            shared_from_this(),
            std::move(id),
            std::move(parent_span_id),
            std::move(name),
        };
    }

    void event(
        std::string span_id,
        std::string parent_span_id,
        std::string name,
        trace_attributes attributes = {})
    {
        publish(
            trace_record{
                .kind = trace_record_kind::event,
                .span_id = std::move(span_id),
                .parent_span_id = std::move(parent_span_id),
                .name = std::move(name),
                .status = {},
                .at = trace_clock::now(),
                .start = {},
                .end = {},
                .attributes = std::move(attributes),
            });
    }

    void finish_span(
        std::string span_id,
        std::string parent_span_id,
        std::string name,
        std::string status)
    {
        auto now = trace_clock::now();
        publish(
            trace_record{
                .kind = trace_record_kind::span_end,
                .span_id = std::move(span_id),
                .parent_span_id = std::move(parent_span_id),
                .name = std::move(name),
                .status = std::move(status),
                .at = now,
                .start = {},
                .end = now,
                .attributes = {},
            });
    }

    /// Adds an observer called for every later record.
    void observe(observer callback)
    {
        auto guard = std::scoped_lock{mutex_};
        observers_.push_back(std::move(callback));
    }

    /// Snapshot of the span with `span_id`, if this context started it.
    [[nodiscard]] std::optional<trace_span_snapshot>
    span(std::string_view span_id) const
    {
        auto guard = std::scoped_lock{mutex_};
        for (const auto & span : spans_) {
            if (span.span_id == span_id)
                return span;
        }
        return std::nullopt;
    }

    /// Snapshots of the spans started under `parent_span_id`, in start
    /// order.
    [[nodiscard]] std::vector<trace_span_snapshot>
    children(std::string_view parent_span_id) const
    {
        auto out = std::vector<trace_span_snapshot>{};
        auto guard = std::scoped_lock{mutex_};
        for (const auto & span : spans_) {
            if (span.parent_span_id == parent_span_id)
                out.push_back(span);
        }
        return out;
    }

private:
    static std::string next_id()
    {
        static auto source = std::atomic<std::uint64_t>{1};
        return std::to_string(source.fetch_add(1, std::memory_order_relaxed));
    }

    void publish(const trace_record & record)
    {
        auto observers = std::vector<observer>{};
        {
            auto guard = std::scoped_lock{mutex_};
            apply(record);
            observers = observers_;
        }
        for (const auto & observer : observers)
            observer(record);
    }

    void apply(const trace_record & record)
    {
        if (record.kind == trace_record_kind::span_begin) {
            spans_.push_back(
                trace_span_snapshot{
                    .span_id = record.span_id,
                    .parent_span_id = record.parent_span_id,
                    .name = record.name,
                    .status = {},
                    .start = record.start,
                    .end = {},
                    .attributes = record.attributes,
                });
            return;
        }

        if (record.kind != trace_record_kind::span_end)
            return;

        for (auto & span : spans_) {
            if (span.span_id == record.span_id) {
                span.status = record.status;
                span.end = record.end;
                return;
            }
        }
    }

    mutable std::mutex mutex_;
    std::vector<trace_span_snapshot> spans_;
    std::vector<observer> observers_;
};

inline void trace_span::event(
    std::string name,
    trace_attributes attributes) const
{
    if (context_ == nullptr)
        return;
    context_->event(
        span_id_, parent_span_id_, std::move(name), std::move(attributes));
}

inline void trace_span::finish(std::string status) const
{
    if (context_ == nullptr)
        return;
    context_->finish_span(
        span_id_, parent_span_id_, name_, std::move(status));
}

/// Runtime environment key holding the current `trace_context`.
struct trace_context_key
{
    using value_type = std::shared_ptr<trace_context>;
    static constexpr auto name = "trace_context";
};

/// Runtime environment key holding the current span id.
struct trace_current_span_key
{
    using value_type = std::string;
    static constexpr auto name = "trace_current_span";
};

/// The running task's trace context, or null when none is set.
inline std::shared_ptr<trace_context> current_trace_context()
{
    if (auto context = env_get<trace_context_key>())
        return *context;
    return {};
}

/// The running task's current span id, or empty when none is set.
inline std::string current_trace_span_id()
{
    if (auto span_id = env_get<trace_current_span_key>())
        return *span_id;
    return {};
}

} // namespace nxtrt
