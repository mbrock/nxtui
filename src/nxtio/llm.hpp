#pragma once

#include <nxtai/responses.hpp>
#include <nxtai/tools.hpp>

/// Compatibility aliases for the old `nxt::io::llm` namespace.
namespace nxt::io::llm {

using protocol_error = nxt::ai::responses::protocol_error;
using openai_responses_request = nxt::ai::responses::openai_responses_request;
using stream_event = nxt::ai::responses::stream_event;
using function_call = nxt::ai::tools::function_call;
using function_tool = nxt::ai::tools::function_tool;
using nxt::ai::responses::input_items_from_request;
using nxt::ai::responses::openai_response_stream;
using nxt::ai::responses::openai_responses_body;
using nxt::ai::responses::openai_responses_http_request;
using nxt::ai::responses::response_id_from_event;
using nxt::ai::tools::function_call_from_item;
using nxt::ai::tools::function_call_output;
using nxt::ai::tools::function_tool_definition;
using nxt::ai::tools::function_tool_definitions;
using nxt::ai::tools::run_function_tool;

} // namespace nxt::io::llm
