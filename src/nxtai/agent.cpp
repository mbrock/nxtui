#include <nxtai/common.hpp>

#include <nxtrt/task.hpp>
#include <nxtrt/ui_runtime.hpp>
#include <nxtui/tui.hpp>
#include <nxtai/tool_tui.hpp>

#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxtai {

void prepare_tool_request(llm_request & request, const tool_registry & tools)
{
    if (nxtai::tools::empty(tools))
        return;
    request.tools = nxtai::tools::function_tool_definitions(tools);
    if (!request.store)
        request.include = {"reasoning.encrypted_content"};
}

void append_stateless_turn(
    std::vector<nxtai::openai::raw_json> & input,
    std::vector<nxtai::openai::raw_json> output_items,
    std::vector<nxtai::openai::raw_json> tool_outputs)
{
    for (auto & item : output_items)
        input.push_back(std::move(item));
    for (auto & output : tool_outputs)
        input.push_back(std::move(output));
}


namespace {

std::vector<std::string> last_lines(std::string_view text, std::size_t max)
{
    auto lines = std::vector<std::string>{};
    auto current = std::string{};
    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current = {};
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty() || lines.empty())
        lines.push_back(std::move(current));

    if (lines.size() > max)
        lines.erase(lines.begin(), lines.begin() + (lines.size() - max));
    return lines;
}

std::string join_lines(const std::vector<std::string> & lines)
{
    auto out = std::string{};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0)
            out += '\n';
        out += lines[i];
    }
    return out;
}

auto header_layout(std::string_view model, std::string_view status)
{
    namespace tt = nxtai::tool_tui;
    auto summary = std::format("{}  {}", model, status);
    auto children = std::vector<nxtui::tui::AnyLayout>{};
    children.reserve(2);
    children.push_back(tt::chip(
        " nxtllm ",
        tt::slate_950,
        tt::amber_300,
        nxtui::Emphasis::bold));
    children.push_back(nxtui::tui::flex_text(
        std::move(summary),
        nxtui::tui::fg(tt::slate_400) | nxtui::tui::bg(tt::band_bg)));
    return nxtui::tui::row(std::move(children));
}

nxtui::tui::AnyLayout assistant_preview_layout(std::string_view assistant_text)
{
    namespace tt = nxtai::tool_tui;
    if (assistant_text.empty())
        return {};
    auto preview = join_lines(last_lines(assistant_text, 8));
    return nxtui::tui::text_lines(
        std::move(preview), nxtui::tui::fg(tt::slate_300));
}

nxtui::tui::AnyLayout agent_layout(
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    nxtui::tui::AnyLayout child)
{
    namespace tt = nxtai::tool_tui;
    auto children = std::vector<nxtui::tui::AnyLayout>{};
    children.reserve(3);
    children.push_back(header_layout(model, status));
    children.push_back(assistant_preview_layout(assistant_text));
    children.push_back(std::move(child));
    return nxtui::tui::surface(
        nxtui::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxtui::DEFAULT_EMPHASIS,
        },
        nxtui::tui::column(std::move(children)));
}

template<typename T>
T take_phase_result(nxtrt::catching_deed<T> deed)
{
    auto result = std::move(deed).get();
    if (result)
        return std::move(*result);
    nxtrt::rethrow(result.error());
}


nxtrt::task<nxtrt::catching_deed<std::vector<function_call_result>>>
spawn_tool_phase_child(
    const tool_registry & tools,
    std::vector<function_call> calls,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    std::chrono::milliseconds settle_delay)
{
    auto child = nxtrt::spawn_widget(
        [&tools,
         calls = std::move(calls),
         settle_delay]() mutable {
            return run_function_tool_batch_ui(
                tools,
                std::move(calls),
                settle_delay);
        });
    co_await nxtrt::draw(
        agent_layout(
            model, status, assistant_text, child.surface()));
    co_return std::move(child).cope();
}

} // namespace

nxtrt::task<std::vector<nxtai::tools::function_call_result>>
run_tool_phase(
    const tool_registry & tools,
    std::vector<nxtai::tools::function_call> calls,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    std::chrono::milliseconds settle_delay)
{
    auto deed = co_await nxtrt::with_zone(
        [&]() mutable {
            return spawn_tool_phase_child(
                tools,
                std::move(calls),
                model,
                status,
                assistant_text,
                settle_delay);
        });
    co_return take_phase_result(std::move(deed));
}

namespace {

nxtrt::task<void> print_assistant_if_terminal(std::string & assistant_text)
{
    if (!nxtrt::has_terminal_surface() || assistant_text.empty())
        co_return;
    co_await nxtrt::print(
        nxtai::tool_tui::assistant_block(
            std::move(assistant_text)));
    assistant_text.clear();
    co_return;
}

} // namespace

nxtrt::task<void> run_agent_loop(
    llm_request request,
    tool_registry tools,
    std::size_t max_steps)
{
    auto original = request;
    auto stateless_input = nxtai::responses::input_items_from_request(request);
    auto assistant_text = std::string{};
    auto status = std::string{"starting"};
    auto model = request.model;
    auto settle_delay = nxtrt::has_terminal_surface()
        ? std::chrono::milliseconds{900}
        : std::chrono::milliseconds{0};
    prepare_tool_request(request, tools);
    co_await nxtrt::draw(
        agent_layout(model, status, assistant_text, nxtui::tui::empty()));

    for (std::size_t step = 0; step < max_steps; ++step) {
        status = std::format("turn {} streaming", step + 1);
        auto stream = co_await run_stream_phase(
            request, model, status, assistant_text);
        auto response = std::move(stream.response);
        assistant_text += stream.assistant_text;
        auto calls = co_await nxtai::tools::read_function_calls_from_items(
            std::move(response.output_items));
        if (calls.empty()) {
            co_await print_assistant_if_terminal(assistant_text);
            status = "done";
            co_await nxtrt::draw(
                agent_layout(
                    model, status, assistant_text, nxtui::tui::empty()));
            co_return;
        }

        if (request.store && !response.response_id)
            throw nxtrt::runtime_error{
                "tool call response had no response id"};

        auto started = std::chrono::steady_clock::now();
        status = std::format("running {} tool call(s)", calls.size());

        auto results = co_await run_tool_phase(
            tools,
            std::move(calls),
            model,
            status,
            assistant_text,
            settle_delay);
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        status = std::format(
            "{} tool call(s) in {}ms", results.size(), elapsed);
        co_await nxtrt::draw(
            agent_layout(model, status, assistant_text, nxtui::tui::empty()));

        auto outputs = nxtai::tools::output_items_from_results(results);
        request = original;
        request.input.clear();
        request.input_items.clear();
        request.previous_response_id.clear();

        if (request.store) {
            request.input_items = std::move(outputs);
            request.previous_response_id = *response.response_id;
        } else {
            append_stateless_turn(
                stateless_input,
                std::move(response.output_items),
                std::move(outputs));
            request.input_items = stateless_input;
        }
        prepare_tool_request(request, tools);
    }

    throw nxtrt::runtime_error{"too many tool call turns"};
}

nxtrt::task<void> run_agent_ui_zone(
    llm_request request,
    tool_registry tools)
{
    try {
        co_await run_agent_loop(
            std::move(request),
            std::move(tools));
        nxtrt::request_ui_shutdown();
    } catch (...) {
        nxtrt::request_ui_shutdown();
        throw;
    }
}



} // namespace nxtai
