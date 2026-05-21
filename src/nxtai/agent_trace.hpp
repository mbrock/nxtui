#pragma once

// Tiny helpers for emitting LLM and tool rows into the runtime's
// Arrow trace stream. Each helper is a no-op when tracing is off, so
// call sites don't need to gate on `trace.enabled()` themselves. Span
// attribution comes from the yard's current scope context so rows
// are correctly nested under whichever yard invoked them.

#include <nxtai/responses.hpp>
#include <nxtai/tools.hpp>
#include <nxtio/arrow.hpp>
#include <nxtio/process.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace nxt::ai::agent_trace {

inline void
stamp_with_span(nxt::ui::yard & self, nxt::io::arrow::trace_row & row)
{
    const auto & ctx = self.scope().context();
    row.span_id = ctx.span_id;
    row.parent_span_id = ctx.parent_span_id;
    row.span_name = ctx.span_name;
}

inline void record_llm_request(
    nxt::ui::yard & self,
    const responses::openai_responses_request & request)
{
    auto & trace = self.runtime().trace();
    if (!trace.enabled())
        return;

    auto body = responses::openai_responses_body(request);
    auto metadata = nlohmann::json{
        {"provider", "openai"},
        {"url", "https://api.openai.com/v1/responses"},
        {"model", request.model},
        {"max_output_tokens", request.max_output_tokens},
        {"reasoning_effort", request.reasoning_effort},
        {"reasoning_summary", request.reasoning_summary},
        {"previous_response_id", request.previous_response_id},
        {"request_body", body},
    };
    nxt::io::arrow::trace_row row;
    row.phase = "llm";
    row.event_type = "request";
    row.data = "POST /v1/responses model=" + request.model;
    row.payload_json = metadata.dump();
    stamp_with_span(self, row);
    trace.add(std::move(row));
}

inline void record_llm_event(
    nxt::ui::yard & self,
    const responses::stream_event & event)
{
    auto & trace = self.runtime().trace();
    if (!trace.enabled())
        return;

    nxt::io::arrow::trace_row row;
    row.phase = "llm";
    row.event_type = event.type;
    row.data = event.type;
    row.payload_json = event.data;
    stamp_with_span(self, row);
    trace.add(std::move(row));
}

inline void
record_tool_call(nxt::ui::yard & self, const tools::function_call & call)
{
    auto & trace = self.runtime().trace();
    if (!trace.enabled())
        return;

    auto payload = nlohmann::json::object();
    payload["call_id"] = call.call_id;
    payload["name"] = call.name;
    payload["arguments"] = call.arguments;
    nxt::io::arrow::trace_row row;
    row.phase = "tool";
    row.event_type = "call";
    row.data = call.name;
    row.payload_json = payload.dump();
    stamp_with_span(self, row);
    trace.add(std::move(row));
}

inline void record_tool_result(
    nxt::ui::yard & self,
    const tools::function_call & call,
    std::string_view output,
    bool is_error)
{
    auto & trace = self.runtime().trace();
    if (!trace.enabled())
        return;

    auto payload = nlohmann::json::object();
    payload["call_id"] = call.call_id;
    payload["name"] = call.name;
    payload["output"] = std::string{output};
    payload["error"] = is_error;
    nxt::io::arrow::trace_row row;
    row.phase = "tool";
    row.event_type = is_error ? "error" : "result";
    row.data = call.name;
    row.payload_json = payload.dump();
    stamp_with_span(self, row);
    trace.add(std::move(row));
}

} // namespace nxt::ai::agent_trace
