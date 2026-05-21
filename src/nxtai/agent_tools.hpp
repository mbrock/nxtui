#pragma once

// Extended tools for nxtllm: filesystem reading, ripgrep search,
// web fetch, and bash. Individual tool implementations live in
// nxtai/tools/*.hpp; this header keeps the existing bundled include.

#include <nxtai/tools.hpp>
#include <nxtai/tools/bash.hpp>
#include <nxtai/tools/grep.hpp>
#include <nxtai/tools/read_file.hpp>
#include <nxtai/tools/web_fetch.hpp>

namespace nxt::ai::agent_tools {

inline auto for_agent(nxt::scheduler & sched)
{
    return tools::tool_set{
        read_file_tool{},
        rg_search_tool{.sched = &sched},
        web_fetch_tool{.sched = &sched},
        bash_tool{.sched = &sched},
    };
}

} // namespace nxt::ai::agent_tools
