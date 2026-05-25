#pragma once

#include <nxt/rt/buffers.hpp>
#include <nxt/rt/subprocess.hpp>
#include <nxt/rt/task.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nxt::ai::tool_process {

using namespace std::chrono_literals;

struct result
{
    nxt::rt::child_result status;
    bool failed = false;
    bool output_too_large = false;
    std::string failure_reason;
    std::string output;
};

struct capture_state
{
    nxt::rt::subprocess::piped_child child;
    result captured;
    bool waited = false;
};

inline nxt::rt::task<void>
finish_child(std::shared_ptr<capture_state> state)
{
    if (state->waited)
        co_return;

    state->child.output.reset();
    state->captured.status =
        co_await nxt::rt::subprocess::terminate_and_wait(state->child, 500ms);
    state->waited = true;
}

inline nxt::rt::task<void>
capture_output(
    std::shared_ptr<capture_state> state,
    std::size_t max_capture_bytes)
{
    auto storage = std::array<std::byte, 4096>{};
    auto source = nxt::rt::fd_source{state->child.output_fd()};

    while (true) {
        auto read = co_await source.read_some(storage);
        if (read.bytes != 0) {
            auto text =
                nxt::rt::as_string_view(std::span{storage}.first(read.bytes));
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
        co_await nxt::rt::subprocess::wait_child(state->child);
    state->waited = true;
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

inline nxt::rt::task<result>
capture(
    std::vector<std::string> argv,
    std::size_t max_capture_bytes = 8 * 1024 * 1024)
{
    auto state = std::make_shared<capture_state>();
    state->child = co_await nxt::rt::subprocess::spawn_piped(std::move(argv));

    co_await nxt::rt::finally(
        capture_output(state, max_capture_bytes),
        [state]() {
            return finish_child(state);
        });

    co_return mark_output_too_large_failed(
        std::move(state->captured),
        max_capture_bytes);
}

} // namespace nxt::ai::tool_process
