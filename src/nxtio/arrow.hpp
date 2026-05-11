#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::io::arrow {

/// One row in a trace stream written as an Arrow IPC table.
struct trace_row
{
    /// Stable id shared by all rows from one run.
    std::string run_id;
    /// Monotonic row sequence number.
    std::int64_t seq = 0;
    /// Milliseconds elapsed since the trace was created.
    std::int64_t elapsed_ms = 0;
    /// Coarse phase, such as `request`, `event`, or an app marker.
    std::string phase;
    /// Detailed event type within the phase.
    std::string event_type;
    /// Short human-readable value for quick inspection.
    std::string data;
    /// Full structured payload as JSON text.
    std::string payload_json;
};

/// In-memory trace collector that writes Arrow IPC files on demand.
class ipc_trace
{
public:
    /// Create an enabled trace when `output_path` is set.
    explicit ipc_trace(
        std::optional<std::string> output_path,
        std::string run_id_prefix = "nxt-trace");
    ~ipc_trace();

    ipc_trace(const ipc_trace &) = delete;
    ipc_trace & operator=(const ipc_trace &) = delete;
    ipc_trace(ipc_trace &&) noexcept;
    ipc_trace & operator=(ipc_trace &&) noexcept;

    /// True when rows are being collected.
    [[nodiscard]] bool enabled() const noexcept;
    /// Destination IPC file, or `std::nullopt` for a disabled trace.
    [[nodiscard]] const std::optional<std::string> & output_path() const;

    /// Append a trace row using the current run id, sequence, and clock.
    void add(
        std::string phase,
        std::string event_type,
        std::string data,
        std::string payload_json);
    /// Write collected rows to `output_path()`.
    void write();

private:
    class writer;

    std::optional<std::string> output_path_;
    std::string run_id_;
    std::chrono::steady_clock::time_point start_;
    std::int64_t next_seq_ = 0;
    std::unique_ptr<writer> writer_;
};

/// Read an Arrow IPC trace file back into row structs.
std::vector<trace_row> read_trace_ipc(std::string_view path);

} // namespace nxt::io::arrow
