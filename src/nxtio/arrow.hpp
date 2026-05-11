#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::io::arrow {

struct trace_row
{
    std::string run_id;
    std::int64_t seq = 0;
    std::int64_t elapsed_ms = 0;
    std::string phase;
    std::string event_type;
    std::string data;
    std::string payload_json;
};

class ipc_trace
{
public:
    explicit ipc_trace(
        std::optional<std::string> output_path,
        std::string run_id_prefix = "nxt-trace");
    ~ipc_trace();

    ipc_trace(const ipc_trace &) = delete;
    ipc_trace & operator=(const ipc_trace &) = delete;
    ipc_trace(ipc_trace &&) noexcept;
    ipc_trace & operator=(ipc_trace &&) noexcept;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] const std::optional<std::string> & output_path() const;

    void add(
        std::string phase,
        std::string event_type,
        std::string data,
        std::string payload_json);
    void write();

private:
    class writer;

    std::optional<std::string> output_path_;
    std::string run_id_;
    std::chrono::steady_clock::time_point start_;
    std::int64_t next_seq_ = 0;
    std::unique_ptr<writer> writer_;
};

std::vector<trace_row> read_trace_ipc(std::string_view path);

} // namespace nxt::io::arrow
