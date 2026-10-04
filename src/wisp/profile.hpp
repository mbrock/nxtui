// SPDX-License-Identifier: AGPL-3.0-or-later
// Semantic counters adapted from mbrock/wisp core/profile.zig at
// a282b936867fcf7d5d2926ebee9afbaf3e153f5b.
#pragma once

#include <array>
#include <cstdint>

namespace wisp {

#if defined(NXT_WISP_PROFILE) && NXT_WISP_PROFILE
inline constexpr bool profile_enabled = true;
#else
inline constexpr bool profile_enabled = false;
#endif

/// Host-local, never serialized. Enable with Meson's -Dwisp_profile=true,
/// then attach to a heap. Disabled builds compile out hot-path recording.
/// Counters describe this evaluator's operations, not a portable bytecode
/// or work unit shared with Zig. Histograms saturate at bucket 16.
struct profile
{
    std::uint64_t evaluator_steps = 0;
    std::uint64_t jet_calls = 0, function_calls = 0, macro_calls = 0;
    std::uint64_t continuation_calls = 0, continuation_searches = 0;
    std::uint64_t continuation_captures = 0, continuation_boundaries = 0;
    std::uint64_t continuation_pushes = 0, arguments_accumulated = 0;
    std::uint64_t continuation_copy_words = 0;
    std::uint64_t lists_scanned = 0, list_cells_scanned = 0;
    std::uint64_t lexical_lookups = 0, lexical_frames = 0;
    std::uint64_t lexical_comparisons = 0, lexical_global_fallbacks = 0;
    std::uint64_t dynamic_lookups = 0, dynamic_hops = 0, dynamic_hits = 0;
    std::array<std::uint64_t, 17> call_arity{}, lexical_depth{};
    std::array<std::uint64_t, 32> allocations{}, gc_copies{};
    std::uint64_t v08_bytes = 0, v32_words = 0;
    std::uint64_t gc_v08_bytes = 0, gc_v32_words = 0;
    std::uint64_t gc_count = 0, gc_nanoseconds = 0;
};

} // namespace wisp
