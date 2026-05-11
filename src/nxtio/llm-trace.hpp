#pragma once

#include <nxtio/llm.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::io::nxtllm {

struct trace_row
{
    std::int64_t seq = 0;
    std::int64_t elapsed_ms = 0;
    std::string phase;
    std::string event_type;
    std::string data;
    std::string payload_json;
};

class arrow_trace
{
public:
    explicit arrow_trace(std::optional<std::string> output_path);
    ~arrow_trace();

    arrow_trace(const arrow_trace &) = delete;
    arrow_trace & operator=(const arrow_trace &) = delete;
    arrow_trace(arrow_trace &&) noexcept;
    arrow_trace & operator=(arrow_trace &&) noexcept;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] const std::optional<std::string> & output_path() const;

    void record_request(const llm::openai_responses_request & request);
    void record_event(const llm::stream_event & event);
    void record_marker(std::string phase, std::string data = {});
    void write();

private:
    class writer;

    void
    add(std::string phase,
        std::string event_type,
        std::string data,
        std::string payload_json);

    std::optional<std::string> output_path_;
    std::string run_id_;
    std::chrono::steady_clock::time_point start_;
    std::int64_t next_seq_ = 0;
    std::unique_ptr<writer> writer_;
};

std::vector<trace_row> read_trace_ipc(std::string_view path);

} // namespace nxt::io::nxtllm
