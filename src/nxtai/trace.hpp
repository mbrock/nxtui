#pragma once

#include <nxtai/responses.hpp>
#include <nxtio/arrow.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::ai::trace {

/// Generic Arrow trace row used by Responses tracing.
using trace_row = nxt::io::arrow::trace_row;

/// Responses-specific trace writer backed by Arrow IPC.
class response_trace
{
public:
    /// Create an enabled trace when `output_path` has a value.
    explicit response_trace(std::optional<std::string> output_path);
    ~response_trace();

    response_trace(const response_trace &) = delete;
    response_trace & operator=(const response_trace &) = delete;
    response_trace(response_trace &&) noexcept;
    response_trace & operator=(response_trace &&) noexcept;

    /// True when rows will be collected and written.
    [[nodiscard]] bool enabled() const noexcept;
    /// Destination IPC file, or `std::nullopt` for a disabled trace.
    [[nodiscard]] const std::optional<std::string> & output_path() const;

    /// Record a sanitized Responses request body and metadata.
    void record_request(const responses::openai_responses_request & request);
    /// Record one streaming Responses event.
    void record_event(const responses::stream_event & event);
    /// Record an application marker row.
    void record_marker(std::string phase, std::string data = {});
    /// Flush collected rows to the IPC file.
    void write();

private:
    nxt::io::arrow::ipc_trace trace_;
};

/// Read a Responses trace IPC file back into rows.
std::vector<trace_row> read_trace_ipc(std::string_view path);

} // namespace nxt::ai::trace
