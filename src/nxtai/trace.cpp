#include <nxtai/trace.hpp>

#include <nxtio/short_id.hpp>

#include <glaze/glaze_exceptions.hpp>

#include <string>
#include <utility>

namespace nxt::ai::trace {

struct response_request_headers
{
    std::string accept = "text/event-stream";
    std::string content_type = "application/json";
    std::string user_agent = "nxtllm/0";

    struct glaze
    {
        using T = response_request_headers;
        static constexpr auto value = glz::object(
            "Accept",
            &T::accept,
            "Content-Type",
            &T::content_type,
            "User-Agent",
            &T::user_agent);
    };
};

struct response_request_payload
{
    std::string provider = "openai";
    std::string api = "responses";
    std::string url = "https://api.openai.com/v1/responses";
    std::string method = "POST";
    responses::openai_responses_body_payload request_body;
    response_request_headers headers;
    bool authorization_header_present = false;
};

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
    auto metadata = response_request_payload{
        .request_body =
            responses::openai_responses_body_payload_from_request(request),
        .authorization_header_present = !request.api_key.empty(),
    };

    trace_.add(
        "request",
        "openai.responses.request",
        glz::ex::write_json(metadata),
        body);
}

void response_trace::record_event(const responses::stream_event & event)
{
    if (!enabled())
        return;

    trace_.add("sse_event", event.type, event.data, event.data);
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
