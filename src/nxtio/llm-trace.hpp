#pragma once

#include <nxtai/trace.hpp>

/// Compatibility aliases for the old `nxt::io::nxtllm` trace namespace.
namespace nxt::io::nxtllm {

using trace_row = nxt::ai::trace::trace_row;
using arrow_trace = nxt::ai::trace::response_trace;
using nxt::ai::trace::read_trace_ipc;

} // namespace nxt::io::nxtllm
