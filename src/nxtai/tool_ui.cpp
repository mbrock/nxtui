#include <nxtai/common.hpp>

#include <nxt/rt/task.hpp>
#include <nxt/rt/ui_runtime.hpp>
#include <nxtai/tool_tui.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nxtai {
namespace {
nxt::rt::task<nxtai::tools::tool_result> run_one_tool_call_worker(
    const tool_registry & tools,
    const nxtai::tools::function_call & call,
    bool & done)
{
    try {
        auto result = co_await nxtai::tools::run_function_tool(tools, call);
        done = true;
        co_return result;
    } catch (...) {
        done = true;
        throw;
    }
}

nxt::rt::task<nxt::rt::catching_deed<tool_result>>
run_one_tool_call_meter(
    const tool_registry & tools,
    const function_call & call,
    std::chrono::steady_clock::time_point started,
    nxtai::tool_tui::call_view & view,
    bool & done)
{
    auto deed = nxt::rt::fork(
        run_one_tool_call_worker(tools, call, done)).cope();

    while (!done) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        view.elapsed_ms = static_cast<int>(elapsed);
        co_await nxt::rt::draw(nxtai::tool_tui::render_call(view));
        co_await nxt::rt::op::timeout::after(frame_interval);
    }
    co_return std::move(deed);
}

nxt::rt::task<nxtai::tools::function_call_result> run_one_tool_call_ui(
    const tool_registry & tools,
    nxtai::tools::function_call call,
    std::chrono::milliseconds settle_delay)
{
    auto started = std::chrono::steady_clock::now();
    auto view = nxtai::tool_tui::call_view{
        .name = call.name,
        .arguments = call.arguments,
        .output = {},
        .latest_memory_current = std::nullopt,
        .state = nxtai::tool_tui::status::running,
        .elapsed_ms = -1,
    };
    co_await nxt::rt::draw(nxtai::tool_tui::render_call(view));

    auto done = false;
    auto worker = co_await nxt::rt::with_zone(
        [&] {
            return run_one_tool_call_meter(
                tools,
                call,
                started,
                view,
                done);
        });

    auto finished = std::move(worker).get();
    if (!finished)
        nxt::rt::rethrow(finished.error());
    auto result = std::move(*finished);
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count();

    view.state = result.failed
        ? nxtai::tool_tui::status::error
        : nxtai::tool_tui::status::ok;
    view.output = result.output;
    if (result.observed && result.observed->latest())
        view.latest_memory_current =
            result.observed->latest()->memory_current.v;
    else
        view.latest_memory_current = std::nullopt;
    view.elapsed_ms = static_cast<int>(elapsed);
    co_await nxt::rt::draw(nxtai::tool_tui::render_call(view));
    if (settle_delay > std::chrono::milliseconds{0})
        co_await nxt::rt::op::timeout::after(settle_delay);

    auto output_item =
        nxtai::tools::function_call_output(
            call.call_id,
            nxtai::tools::tool_result_json(result));
    co_return nxtai::tools::function_call_result{
        .call = std::move(call),
        .result = std::move(result),
        .output_item = std::move(output_item),
    };
}

nxt::rt::task<nxtai::tools::function_call_result>
run_one_tool_call_counted(
    const tool_registry & tools,
    nxtai::tools::function_call call,
    std::chrono::milliseconds settle_delay,
    std::size_t & done_count)
{
    try {
        auto result = co_await run_one_tool_call_ui(
            tools,
            std::move(call),
            settle_delay);
        ++done_count;
        co_return result;
    } catch (...) {
        ++done_count;
        throw;
    }
}

nxt::rt::task<std::vector<nxt::rt::catching_deed<function_call_result>>>
spawn_tool_call_children(
    const tool_registry & tools,
    std::vector<function_call> & calls,
    std::vector<nxt::rt::widget_slot> & surfaces,
    std::chrono::milliseconds settle_delay,
    std::size_t & done_count)
{
    auto out = std::vector<nxt::rt::catching_deed<function_call_result>>{};
    out.reserve(calls.size());

    surfaces.reserve(calls.size());
    for (auto & call : calls) {
        auto child = nxt::rt::spawn_widget(
            [&tools,
             call = std::move(call),
             settle_delay,
             &done_count]() mutable {
                return run_one_tool_call_counted(
                    tools,
                    std::move(call),
                    settle_delay,
                    done_count);
            });
        surfaces.emplace_back(child.surface());
        out.push_back(std::move(child).cope());
    }
    co_await nxt::rt::draw(nxt::rt::child_slots_column(surfaces));

    while (done_count < out.size())
        co_await nxt::rt::op::timeout::after(frame_interval);
    co_return out;
}

} // namespace

nxt::rt::task<std::vector<nxtai::tools::function_call_result>>
run_function_tool_batch_ui(
    const tool_registry & tools,
    std::vector<nxtai::tools::function_call> calls,
    std::chrono::milliseconds settle_delay)
{
    if (!nxt::rt::has_terminal_surface()) {
        co_await nxt::rt::write_stdout_all(
            std::format(
                "[tools] running {} tool call(s)\n", calls.size()));
        auto results = co_await nxtai::tools::run_function_tool_batch(
            tools,
            std::move(calls));
        for (const auto & result : results) {
            co_await nxt::rt::write_stdout_all(
                std::format(
                    "[tool] {} -> {} ({} bytes)\n",
                    result.call.name,
                    result.result.failed ? "failed" : "ok",
                    result.result.output.size()));
        }
        co_return results;
    }

    auto done_count = std::size_t{};
    auto surfaces = std::vector<nxt::rt::widget_slot>{};
    auto deeds = co_await nxt::rt::with_zone(
        [&] {
            return spawn_tool_call_children(
                tools,
                calls,
                surfaces,
                settle_delay,
                done_count);
        });

    try {
        auto out = std::vector<nxtai::tools::function_call_result>{};
        out.reserve(deeds.size());
        for (auto & deed : deeds) {
            auto result = std::move(deed).get();
            if (result) {
                out.push_back(std::move(*result));
            } else {
                nxt::rt::rethrow(result.error());
            }
        }
        nxt::rt::clear_widget();
        co_return out;
    } catch (...) {
        nxt::rt::clear_widget();
        throw;
    }
}


} // namespace nxtai
