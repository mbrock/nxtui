#pragma once

#include <cstdint>
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

void write_trace_parquet(
    std::string_view path,
    std::string_view run_id,
    const std::vector<trace_row> & rows);

std::vector<trace_row> read_trace_parquet(std::string_view path);

} // namespace nxt::io::nxtllm
