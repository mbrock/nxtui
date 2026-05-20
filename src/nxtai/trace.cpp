#include <nxtai/trace.hpp>

#include <nxtio/short_id.hpp>

#include <nlohmann/json.hpp>

#include <utility>

namespace nxt::ai::trace {

response_trace::response_trace(std::optional<std::string> output_path)
    : trace_(std::move(output_path), "nxtllm-" + nxt::io::make_short_id())
{
}

response_trace::~response_trace() = default;

bool response_trace::enabled() const noexcept
{
    return trace_.enabled();
}

const std::optional<std::string> & response_trace::output_path() const
{
    return trace_.output_path();
}

void response_trace::record_request(
    const responses::openai_responses_request & request)
{
    if (!enabled())
        return;

    auto body = responses::openai_responses_body(request);
    auto metadata = nlohmann::json{
        {"provider", "openai"},
        {"api", "responses"},
        {"url", "https://api.openai.com/v1/responses"},
        {"method", "POST"},
        {"request_body", body},
        {"headers",
         {
             {"Accept", "text/event-stream"},
             {"Content-Type", "application/json"},
             {"User-Agent", "nxtllm/0"},
         }},
        {"authorization_header_present", !request.api_key.empty()},
    };

    trace_.add(
        "request",
        "openai.responses.request",
        metadata.dump(),
        body.dump());
}

void response_trace::record_event(const responses::stream_event & event)
{
    if (!enabled())
        return;

    trace_.add("sse_event", event.type, event.raw, event.raw);
}

void response_trace::record_marker(std::string phase, std::string data)
{
    if (!enabled())
        return;

    trace_.add(std::move(phase), {}, std::move(data), {});
}

void response_trace::write()
{
    trace_.write();
}

std::vector<trace_row> read_trace_ipc(std::string_view path)
{
    return nxt::io::arrow::read_trace_ipc(path);
}

} // namespace nxt::ai::trace
