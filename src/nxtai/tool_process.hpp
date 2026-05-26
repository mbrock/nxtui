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

namespace nxtai::tool_process {

using namespace std::chrono_literals;

struct result
{
    nxtrt::child_result status;
    nxtrt::scoped_process::observation observed;
    bool failed = false;
    bool output_too_large = false;
    std::string failure_reason;
    std::string output;
};

struct capture_state
{
    nxtrt::subprocess::piped_child child;
    nxtrt::scoped_process::observation observed;
    result captured;
    bool waited = false;
    bool done = false;
};

struct capture_options
{
    std::size_t max_capture_bytes = 8 * 1024 * 1024;
    nxtrt::scoped_process::options scope = {};
};

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

inline nxtrt::task<void>
capture_output(
    std::shared_ptr<capture_state> state,
    std::size_t max_capture_bytes)
{
    auto storage = std::array<std::byte, 4096>{};
    auto source = nxtrt::fd_source{state->child.output_fd()};

    while (true) {
        auto read = co_await source.read_some(storage);
        if (read.bytes != 0) {
            auto text =
                nxtrt::as_string_view(std::span{storage}.first(read.bytes));
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
        if (read.eof)
            break;
    }

    state->child.output.reset();
    state->captured.status =
        co_await nxtrt::subprocess::wait_child(state->child);
    state->waited = true;
    state->done = true;
}

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

    auto capture = nxtrt::catching_deed<void>{};
    auto monitor = nxtrt::catching_deed<void>{};
    co_await nxtrt::with_zone([&]() -> nxtrt::task<void> {
        capture =
            nxtrt::fork(
                nxtrt::detail::stop_zone_on_completion(
                    nxtrt::finally(
                        capture_output(state, options.max_capture_bytes),
                        [state]() {
                            return finish_child(state);
                        })))
                .cope();
        monitor =
            nxtrt::fork(
                nxtrt::scoped_process::monitor_until_done(
                    state->observed,
                    state->done,
                    options.scope.sample_interval,
                    options.scope.max_samples))
                .cope();
        co_return;
    });

    auto capture_done = std::move(capture).get();
    if (!capture_done)
        nxtrt::rethrow(capture_done.error());
    auto monitor_done = std::move(monitor).get();
    if (!monitor_done)
        nxtrt::rethrow(monitor_done.error());

    state->captured.observed = std::move(state->observed);
    co_return mark_output_too_large_failed(
        std::move(state->captured),
        options.max_capture_bytes);
}

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
