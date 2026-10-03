#pragma once

#include <nxtrt/buffers.hpp>
#include <nxtrt/scoped_process.hpp>
#include <nxtrt/subprocess.hpp>
#include <nxtrt/task.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

/**
 * @namespace nxtai::tool_process
 * Run a child process for a tool and capture its combined stdout and
 * stderr, with a size cap, cleanup on cancellation, and optional cgroup
 * sampling when the child runs in a systemd user scope. `capture` is the
 * entry point.
 */
namespace nxtai::tool_process {

using namespace std::chrono_literals;

/// Outcome of `capture`.
struct result
{
    nxtrt::child_result status;
    nxtrt::scoped_process::observation observed;
    bool failed = false;
    bool output_too_large = false;
    std::string failure_reason;
    std::string output;
};

/// Shared state between the capture and monitor jobs of one `capture`.
struct capture_state
{
    nxtrt::subprocess::piped_child child;
    nxtrt::scoped_process::observation observed;
    result captured;
    bool waited = false;
    bool done = false;
};

/// Options for `capture`.
struct capture_options
{
    std::size_t max_capture_bytes = 8 * 1024 * 1024;
    nxtrt::scoped_process::options scope = {};
};

/// Cleanup for `capture`: if the child has not been waited for, close the
/// pipe and terminate it, escalating after 500 ms, then reap it.
inline nxtrt::task<void>
finish_child(std::shared_ptr<capture_state> state)
{
    if (state->waited)
        co_return;

    state->child.output.reset();
    state->captured.status =
        co_await nxtrt::subprocess::terminate_and_wait(state->child, 500ms);
    state->waited = true;
    state->done = true;
}

/// Read the child's output until EOF, keeping at most `max_capture_bytes`
/// and flagging any excess, then wait for the child to exit.
inline nxtrt::task<void>
capture_output(
    std::shared_ptr<capture_state> state,
    std::size_t max_capture_bytes)
{
    auto storage = std::array<std::byte, 4096>{};
    auto source = nxtrt::fd_source{
        state->child.output_fd(),
        std::span{storage},
    };

    while (true) {
        auto chunk = co_await source.take_some();
        if (chunk && !chunk->empty()) {
            auto text = nxtrt::as_string_view(*chunk);
            auto & output = state->captured.output;
            if (output.size() < max_capture_bytes) {
                auto remaining = max_capture_bytes - output.size();
                auto copied = std::min(remaining, text.size());
                output.append(text.substr(0, copied));
                if (copied != text.size())
                    state->captured.output_too_large = true;
            } else {
                state->captured.output_too_large = true;
            }
        }
        if (!chunk)
            break;
    }

    state->child.output.reset();
    state->captured.status =
        co_await nxtrt::subprocess::wait_child(state->child);
    state->waited = true;
    state->done = true;
}

/// If the output was truncated, mark the result failed and prefix the
/// captured output with a message naming the limit.
inline result mark_output_too_large_failed(result captured, std::size_t max_bytes)
{
    if (!captured.output_too_large)
        return captured;

    captured.failed = true;
    captured.failure_reason =
        "tool output exceeded capture limit ("
        + std::to_string(max_bytes)
        + " bytes)";
    auto message =
        captured.failure_reason
        + "; captured prefix follows:\n"
        + std::move(captured.output);
    captured.output = std::move(message);
    return captured;
}

/// Spawn `argv` (looked up on `PATH`) with stdout and stderr on one pipe,
/// read all output, and wait for the child.
///
/// Output beyond `options.max_capture_bytes` (8 MiB by default) is
/// discarded but still drained; the result is then marked failed with an
/// explanatory prefix. A nonzero exit status does not set `failed`; check
/// `status`. With `options.scope.systemd_user_scope`, the child runs in a
/// transient systemd scope and its cgroup is sampled until it exits.
///
/// If the awaiting task is stopped or the read fails, the child is
/// terminated and reaped before the exception propagates. Spawn failures
/// throw.
inline nxtrt::task<result>
capture(
    std::vector<std::string> argv,
    capture_options options = {})
{
    auto state = std::make_shared<capture_state>();
    auto child =
        co_await nxtrt::scoped_process::spawn_piped(
            std::move(argv),
            std::move(options.scope));
    state->child = std::move(child.child);
    state->observed = std::move(child.observed);

    // The monitor stops once the capture (the first job) settles.
    auto outcomes = co_await nxtrt::settle(
        std::tuple{
            nxtrt::finally(
                capture_output(state, options.max_capture_bytes),
                [state] { return finish_child(state); }),
            nxtrt::scoped_process::monitor_until_done(
                state->observed,
                state->done,
                options.scope.sample_interval,
                options.scope.max_samples),
        },
        nxtrt::primary_group{});

    auto capture_done = std::move(std::get<0>(outcomes));
    if (!capture_done)
        nxtrt::rethrow(capture_done.error());
    auto monitor_done = std::move(std::get<1>(outcomes));
    if (!monitor_done)
        nxtrt::rethrow(monitor_done.error());

    state->captured.observed = std::move(state->observed);
    co_return mark_output_too_large_failed(
        std::move(state->captured),
        options.max_capture_bytes);
}

/// `capture` with only a byte limit.
inline nxtrt::task<result>
capture(
    std::vector<std::string> argv,
    std::size_t max_capture_bytes)
{
    co_return co_await capture(
        std::move(argv),
        capture_options{.max_capture_bytes = max_capture_bytes});
}

} // namespace nxtai::tool_process
