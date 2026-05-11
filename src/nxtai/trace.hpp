#pragma once

#include <nxtai/responses.hpp>
#include <nxtio/arrow.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::ai::trace {

using trace_row = nxt::io::arrow::trace_row;

class response_trace
{
public:
    explicit response_trace(std::optional<std::string> output_path);
    ~response_trace();

    response_trace(const response_trace &) = delete;
    response_trace & operator=(const response_trace &) = delete;
    response_trace(response_trace &&) noexcept;
    response_trace & operator=(response_trace &&) noexcept;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] const std::optional<std::string> & output_path() const;

    void record_request(const responses::openai_responses_request & request);
    void record_event(const responses::stream_event & event);
    void record_marker(std::string phase, std::string data = {});
    void write();

private:
    nxt::io::arrow::ipc_trace trace_;
};

std::vector<trace_row> read_trace_ipc(std::string_view path);

} // namespace nxt::ai::trace
