#pragma once

#include <nxt/rt/task.hpp>
#include <nxt/llm/openai_types.hpp>
#include <nxt/llm/responses_request.hpp>
#include <nxt/llm/tool_batch.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::llm {

using llm_request = nxt::llm::responses::openai_responses_request;
using function_call = nxt::llm::tools::function_call;
using function_call_result = nxt::llm::tools::function_call_result;
using tool_result = nxt::llm::tools::tool_result;
using tool_registry = nxt::llm::tools::tool_registry;

constexpr std::size_t default_max_output_tokens = 128000;
constexpr auto frame_interval = std::chrono::milliseconds{16};

struct response_stream_result
{
    std::vector<nxt::llm::openai::raw_json> output_items;
    std::optional<std::string> response_id;
    bool completed = false;
};

struct stream_phase_result
{
    response_stream_result response;
    std::string assistant_text;
};

void prepare_tool_request(llm_request & request, const tool_registry & tools);

nxt::rt::task<stream_phase_result> stream_openai_response(
    const llm_request & request);

nxt::rt::task<stream_phase_result> run_stream_phase(
    const llm_request & request,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text);

nxt::rt::task<std::vector<nxt::llm::tools::function_call_result>>
run_function_tool_batch_ui(
    const tool_registry & tools,
    std::vector<nxt::llm::tools::function_call> calls,
    std::chrono::milliseconds settle_delay);

nxt::rt::task<std::vector<nxt::llm::tools::function_call_result>>
run_tool_phase(
    const tool_registry & tools,
    std::vector<nxt::llm::tools::function_call> calls,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    std::chrono::milliseconds settle_delay);

nxt::rt::task<void> run_agent_loop(
    llm_request request,
    tool_registry tools,
    std::size_t max_steps = 32);

nxt::rt::task<void> run_agent_ui_zone(
    llm_request request,
    tool_registry tools);

} // namespace nxt::llm
