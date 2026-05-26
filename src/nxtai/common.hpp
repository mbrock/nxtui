#pragma once

#include <nxtrt/task.hpp>
#include <nxtai/openai_types.hpp>
#include <nxtai/responses_request.hpp>
#include <nxtai/tool_batch.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxtai {

using llm_request = nxtai::responses::openai_responses_request;
using function_call = nxtai::tools::function_call;
using function_call_result = nxtai::tools::function_call_result;
using tool_result = nxtai::tools::tool_result;
using tool_registry = nxtai::tools::tool_registry;

constexpr std::size_t default_max_output_tokens = 128000;
constexpr auto frame_interval = std::chrono::milliseconds{16};

struct response_stream_result
{
    std::vector<nxtai::openai::raw_json> output_items;
    std::optional<std::string> response_id;
    bool completed = false;
};

struct stream_phase_result
{
    response_stream_result response;
    std::string assistant_text;
};

void prepare_tool_request(llm_request & request, const tool_registry & tools);

nxtrt::task<stream_phase_result> stream_openai_response(
    const llm_request & request);

nxtrt::task<stream_phase_result> run_stream_phase(
    const llm_request & request,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text);

nxtrt::task<std::vector<nxtai::tools::function_call_result>>
run_function_tool_batch_ui(
    const tool_registry & tools,
    std::vector<nxtai::tools::function_call> calls,
    std::chrono::milliseconds settle_delay);

nxtrt::task<std::vector<nxtai::tools::function_call_result>>
run_tool_phase(
    const tool_registry & tools,
    std::vector<nxtai::tools::function_call> calls,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    std::chrono::milliseconds settle_delay);

nxtrt::task<void> run_agent_loop(
    llm_request request,
    tool_registry tools,
    std::size_t max_steps = 32);

nxtrt::task<void> run_agent_ui_zone(
    llm_request request,
    tool_registry tools);

} // namespace nxtai
