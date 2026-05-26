#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nxt/json.hpp>
#include <nxt/llm/common.hpp>
#include <nxt/rt/app.hpp>
#include <nxt/rt/task.hpp>
#include <nxt/rt/trace.hpp>
#include <nxt/rt/ui_runtime.hpp>
#include <nxt/llm/responses_request.hpp>
#include <nxt/llm/tool_tui.hpp>
