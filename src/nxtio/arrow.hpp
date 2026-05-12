#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::io::arrow {

/// One row in a trace stream written as an Arrow IPC table. Rows form
/// a single mixed stream that records LLM events, span begin/end
/// markers, raster frames, input events, and free-form application
/// markers — all distinguishable by `phase` plus the `payload_kind`
/// tag.
struct trace_row
{
    /// Stable id shared by all rows from one run.
    std::string run_id;
    /// Monotonic row sequence number.
    std::int64_t seq = 0;
    /// Milliseconds elapsed since the trace was created.
    std::int64_t elapsed_ms = 0;
    /// Coarse phase, such as `span_begin`, `frame`, `sse_event`, or an
    /// app marker.
    std::string phase;
    /// Detailed event type within the phase.
    std::string event_type;
    /// Short human-readable value for quick inspection.
    std::string data;
    /// Structured payload as JSON text. Empty when the payload is
    /// binary or absent.
    std::string payload_json;
    /// Id of the enclosing span (empty when not inside a span).
    std::string span_id;
    /// Id of the parent of `span_id` (empty when the span has no
    /// parent — used for `span_begin` rows so the tree can be
    /// reconstructed without scanning the whole stream).
    std::string parent_span_id;
    /// Human-readable name of `span_id` (carried on every row so
    /// readers don't have to join with `span_begin`).
    std::string span_name;
    /// Frame number for raster/vterm rows; -1 when not applicable.
    std::int64_t frame_seq = -1;
    /// Tag for `payload_bin` contents (e.g. `raster`, `vterm_screen`,
    /// `png`). Empty when no binary payload is attached.
    std::string payload_kind;
    /// Opaque binary payload (raster cells, encoded image bytes, …).
    std::vector<std::uint8_t> payload_bin;
};

/// In-memory trace collector that writes Arrow IPC files on demand.
class ipc_trace
{
public:
    /// Create an enabled trace when `output_path` is set.
    explicit ipc_trace(
        std::optional<std::string> output_path,
        std::string run_id = {});
    ~ipc_trace();

    ipc_trace(const ipc_trace &) = delete;
    ipc_trace & operator=(const ipc_trace &) = delete;
    // Non-movable: the per-instance mutex would not survive a move.
    ipc_trace(ipc_trace &&) = delete;
    ipc_trace & operator=(ipc_trace &&) = delete;

    /// True when rows are being collected.
    [[nodiscard]] bool enabled() const noexcept;
    /// Destination IPC file, or `std::nullopt` for a disabled trace.
    [[nodiscard]] const std::optional<std::string> & output_path() const;
    /// Run id shared by every row in this trace.
    [[nodiscard]] std::string_view run_id() const noexcept;

    /// Append a basic JSON-only trace row using the current run id,
    /// sequence, and clock. Convenience for callers that don't need to
    /// stamp span or frame metadata.
    void add(
        std::string phase,
        std::string event_type,
        std::string data,
        std::string payload_json);

    /// Append a row with full control over span/frame/binary metadata.
    /// `run_id`, `seq`, and `elapsed_ms` on the passed row are ignored
    /// and overwritten — they are owned by the collector.
    void add(trace_row row);

    /// Write collected rows to `output_path()`.
    void write();

private:
    class writer;

    std::optional<std::string> output_path_;
    std::string run_id_;
    std::chrono::steady_clock::time_point start_;
    std::int64_t next_seq_ = 0;
    std::unique_ptr<writer> writer_;
    // Serializes append() and close(). Producers run on libcoro's
    // scheduler thread pool, so without this two coroutines can be
    // mid-batch in the writer simultaneously and corrupt the
    // flatbuffer builder state.
    mutable std::mutex mutex_;
};

/// Read an Arrow IPC trace file back into row structs.
std::vector<trace_row> read_trace_ipc(std::string_view path);

} // namespace nxt::io::arrow
