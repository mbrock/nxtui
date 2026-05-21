#pragma once

// Pretty tool-execution UI for nxtllm.
//
// Each tool call runs as a child "process" with two coroutines:
//   - a worker that actually invokes the tool
//   - a companion that draws an animated card (spinner + name + args)
//
// While the worker runs, the companion's surface is published into
// the agent's HUD via the `accompany` pattern (see nxtio/process.hpp).
// When the tool finishes, a styled-text "done" card is committed to
// scrollback so it leaves a permanent visible record of what ran.

#include "nxt/baltics.hpp"
#include <nxt/ansi.hpp>
#include <nxt/any_layout.hpp>
#include <nxt/tui.hpp>
#include <nxtai/agent_trace.hpp>
#include <nxtai/hud_blocks.hpp>
#include <nxtai/tools.hpp>
#include <nxtio/input.hpp>
#include <nxtio/process.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxt::ai::tool_ui {

using namespace std::chrono_literals;

// ============================================================================
// Display metadata projected from the concrete tool type
// ============================================================================

struct tool_display
{
    std::string_view icon = "tool";
    nxt::Rgba8 color = nxt::Rgba8{nxt::theme::baltic_church.green};
    bool needs_approval = false;
};

[[nodiscard]] inline const nxt::theme::Palette & tool_palette()
{
    return nxt::theme::baltic_church;
}

[[nodiscard]] inline tool_display
default_tool_display(const nxt::theme::Palette & palette = tool_palette())
{
    return tool_display{
        .icon = "tool",
        .color = nxt::Rgba8{palette.green},
        .needs_approval = false,
    };
}

template<typename Tool>
concept has_tool_icon = requires {
    { Tool::icon } -> std::convertible_to<std::string_view>;
};

template<typename Tool>
concept has_theme_color = requires(const nxt::theme::Palette & palette) {
    { Tool::theme_color(palette) } -> std::convertible_to<nxt::Rgba8>;
};

template<typename Tool>
concept has_approval_requirement = requires {
    { Tool::needs_approval } -> std::convertible_to<bool>;
};

template<typename T>
[[nodiscard]] inline std::optional<T>
read_json_view(const std::string & json)
{
    auto value = T{};
    if (glz::read<nxt::ai::openai::json_read_opts>(value, json))
        return std::nullopt;
    return value;
}

template<typename Tool>
concept has_parameters_summary =
    requires(const typename Tool::parameters & parameters) {
        { Tool::parameters_summary(parameters) } -> std::same_as<std::string>;
    };

inline std::string fallback_args_summary(const std::string & args_json)
{
    return args_json.size() > 50 ? args_json.substr(0, 47) + "..."
                                 : args_json;
}

template<typename Tool>
[[nodiscard]] inline std::optional<std::string> args_summary_for_tool(
    std::string_view name,
    const std::string & args_json)
{
    using tool_t = std::remove_cvref_t<Tool>;
    if (name != tool_t::name)
        return std::nullopt;

    auto args = read_json_view<typename tool_t::parameters>(args_json);
    if (!args)
        return fallback_args_summary(args_json);

    if constexpr (has_parameters_summary<tool_t>)
        return tool_t::parameters_summary(*args);
    else
        return fallback_args_summary(args_json);
}

template<std::size_t I = 0, typename ToolSet>
[[nodiscard]] inline std::optional<std::string>
args_summary_for_known_tool(
    const ToolSet & tool_list,
    std::string_view name,
    const std::string & args_json)
{
    using tuple_t = std::remove_cvref_t<decltype(tool_list.items)>;
    if constexpr (I == std::tuple_size_v<tuple_t>) {
        return std::nullopt;
    } else {
        using tool_t = std::tuple_element_t<I, tuple_t>;
        if (auto summary = args_summary_for_tool<tool_t>(name, args_json))
            return summary;
        return args_summary_for_known_tool<I + 1>(
            tool_list, name, args_json);
    }
}

// Short, glanceable summary of the call's args (the value of the
// "main" parameter, usually).
template<typename ToolSet>
inline std::string
args_summary(const ToolSet & tool_list, const tools::function_call & call)
{
    if (auto summary =
            args_summary_for_known_tool(tool_list, call.name, call.arguments))
        return *summary;
    return fallback_args_summary(call.arguments);
}

template<typename Tool>
[[nodiscard]] inline std::optional<tool_display> display_for_tool(
    std::string_view name,
    const nxt::theme::Palette & palette)
{
    using tool_t = std::remove_cvref_t<Tool>;
    if (name != tool_t::name)
        return std::nullopt;

    auto display = default_tool_display(palette);
    if constexpr (has_tool_icon<tool_t>)
        display.icon = tool_t::icon;
    if constexpr (has_theme_color<tool_t>)
        display.color = tool_t::theme_color(palette);
    if constexpr (has_approval_requirement<tool_t>)
        display.needs_approval = tool_t::needs_approval;
    return display;
}

template<std::size_t I = 0, typename ToolSet>
[[nodiscard]] inline std::optional<tool_display>
display_for_known_tool(
    const ToolSet & tool_list,
    std::string_view name,
    const nxt::theme::Palette & palette = tool_palette())
{
    using tuple_t = std::remove_cvref_t<decltype(tool_list.items)>;
    if constexpr (I == std::tuple_size_v<tuple_t>) {
        return std::nullopt;
    } else {
        using tool_t = std::tuple_element_t<I, tuple_t>;
        if (auto display = display_for_tool<tool_t>(name, palette))
            return display;
        return display_for_known_tool<I + 1>(tool_list, name, palette);
    }
}

template<typename ToolSet>
[[nodiscard]] inline tool_display
display_for_call(
    const ToolSet & tool_list,
    const tools::function_call & call,
    const nxt::theme::Palette & palette = tool_palette())
{
    if (auto display = display_for_known_tool(tool_list, call.name, palette))
        return *display;
    return default_tool_display(palette);
}

// ============================================================================
// Cards
// ============================================================================

constexpr std::array<std::string_view, 10> spinner_frames = {
    "⠋",
    "⠙",
    "⠹",
    "⠸",
    "⠼",
    "⠴",
    "⠦",
    "⠧",
    "⠇",
    "⠏",
};

inline auto running_card(
    tool_display display,
    std::string_view args,
    int tick,
    std::chrono::milliseconds elapsed)
{
    using namespace nxt::tui;
    auto frame = std::string{
        spinner_frames
            [static_cast<std::size_t>(tick) % spinner_frames.size()]};
    auto label = std::string{display.icon};
    auto args_str = std::string{args};
    if (args_str.size() > 60)
        args_str = args_str.substr(0, 58) + "…";
    auto elapsed_str =
        elapsed.count() >= 100
            ? std::format("{:.1f}s", elapsed.count() / 1000.0)
            : std::string{};
    return hud_blocks::header_row(
        frame, label, args_str, elapsed_str, display.color, faint);
}

inline auto done_card(
    tool_display display,
    std::string_view args,
    std::chrono::milliseconds elapsed,
    std::string_view summary,
    bool error)
{
    using namespace nxt::tui;
    auto color = error ? nxt::Rgba8{tool_palette().coral} : display.color;
    auto label = std::string{display.icon};
    auto args_str = std::string{args};
    if (args_str.size() > 60)
        args_str = args_str.substr(0, 58) + "…";
    auto time_str = elapsed.count() < 1000
                        ? std::format("{}ms", elapsed.count())
                        : std::format("{:.2f}s", elapsed.count() / 1000.0);
    auto meta = std::string{time_str};
    if (!summary.empty())
        meta += "  " + std::string{summary};
    return hud_blocks::header_row(
        error ? "!" : "✓", label, args_str, meta, color, faint);
}

inline auto folded_result_card(
    tool_display display,
    std::string_view args,
    std::chrono::milliseconds elapsed,
    std::string_view summary,
    bool error)
{
    return done_card(display, args, elapsed, summary, error);
}

// Two-line card for approval prompts. Line 1 names the tool and its
// args; line 2 shows the key hints.
inline auto approval_card(tool_display display, std::string_view args)
{
    using namespace nxt::tui;
    auto warn = nxt::Rgba8{tool_palette().amber};
    auto label = std::string{display.icon};
    auto args_str = std::string{args};
    args_str = args_str.substr(0, 40) + "…";
    return column(
        styled_text(
            span(" !  ", fg(warn) | bold),
            span("approve  ", fg(warn) | bold),
            span(label + "  ", fg(display.color) | bold),
            span(args_str, fg(nxt::Rgba8{tool_palette().fg}))),
        styled_text(
            span("    ", fg(warn)),
            span("y", fg(warn) | bold),
            span(" run  ", faint),
            span("n", fg(warn) | bold),
            span(" deny  ", faint),
            span("A", fg(warn) | bold),
            span(" all  ", faint),
            span("D", fg(warn) | bold),
            span(" deny all  ", faint),
            span("ESC", fg(warn) | bold),
            span(" cancel batch", faint)));
}

inline auto denied_card(tool_display display, std::string_view args)
{
    using namespace nxt::tui;
    auto color = nxt::Rgba8{tool_palette().coral};
    auto label = std::string{display.icon};
    auto args_str = std::string{args};
    if (args_str.size() > 70)
        args_str = args_str.substr(0, 68) + "…";
    return hud_blocks::header_row(
        "N", label, args_str, "denied", color, faint);
}

// Compact "queued" indicator drawn into each child's surface while
// approvals are still being collected for the batch.
inline auto queued_card(tool_display display, std::string_view args)
{
    using namespace nxt::tui;
    auto color = nxt::Rgba8{tool_palette().fg_subtle};
    auto label = std::string{display.icon};
    auto args_str = std::string{args};
    if (args_str.size() > 70)
        args_str = args_str.substr(0, 68) + "…";
    return hud_blocks::header_row(
        "·", label, args_str, "queued", color, faint);
}

// One-line summary of the result (bytes/error).
inline std::string result_summary(const tools::tool_result & result)
{
    if (result.failed)
        return "error: " + result.output;
    if (!result.output.empty())
        return std::format("{} bytes", result.output.size());
    return {};
}

// Strip terminal control characters from a line before we
// self.print it into scrollback. Without this, raw bash output
// (especially from any tool that doesn't honor isatty) can embed
// CR/ESC/control sequences that retarget the cursor, reset the
// scroll region, or otherwise corrupt the HUD's geometry — at which
// point the runtime's HUD region math diverges from what the
// terminal believes is reserved, and the visible HUD "collapses".
inline std::string sanitize_line(std::string_view line)
{
    std::string out;
    out.reserve(line.size());
    std::size_t i = 0;
    while (i < line.size()) {
        auto c = static_cast<unsigned char>(line[i]);
        if (c == 0x1b) {
            // Skip an ESC + the next byte at minimum; for CSI
            // sequences eat up to the final byte (0x40..0x7e).
            ++i;
            if (i < line.size() && line[i] == '[') {
                ++i;
                while (i < line.size()) {
                    auto x = static_cast<unsigned char>(line[i]);
                    ++i;
                    if (x >= 0x40 && x <= 0x7e)
                        break;
                }
            } else {
                ++i;
            }
            continue;
        }
        if (c == '\r') {
            // CR alone retargets the line cursor; ignore.
            ++i;
            continue;
        }
        if (c == '\t') {
            out += "    ";
            ++i;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            // Other C0 controls — drop silently. (A visible
            // marker is tempting but would only flag rare cases;
            // in practice the safest thing is to elide them so
            // there's no chance of leaking partial sequences into
            // the outer terminal state.)
            ++i;
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
}

// Up to `max_lines` preview lines extracted from the result payload.
// Each line is trimmed to ~120 cells and sanitized so embedded terminal
// controls can't escape into the outer terminal state.
inline std::vector<std::string>
preview_lines(const tools::tool_result & result, std::size_t max_lines = 4)
{
    std::vector<std::string> out;
    if (result.failed)
        return out;
    const auto & content = result.output;
    if (content.empty())
        return out;
    std::size_t pos = 0;
    while (pos < content.size() && out.size() < max_lines) {
        auto nl = content.find('\n', pos);
        auto end = nl == std::string::npos ? content.size() : nl;
        auto raw = std::string_view{content.data() + pos, end - pos};
        auto line = sanitize_line(raw);
        if (line.size() > 120)
            line = line.substr(0, 118) + "…";
        if (!line.empty())
            out.push_back(std::move(line));
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return out;
}

// Total number of payload lines (for the "and N more lines" hint
// when preview is truncated).
inline std::size_t result_total_lines(const tools::tool_result & result)
{
    const auto & content = result.output;
    if (content.empty())
        return 0;
    return static_cast<std::size_t>(
               std::count(content.begin(), content.end(), '\n'))
           + (content.back() == '\n' ? 0 : 1);
}

inline std::vector<nxt::tui::Span>
preview_spans(const tools::tool_result & result, std::size_t max_lines = 4)
{
    using namespace nxt::tui;
    auto preview = preview_lines(result, max_lines);
    auto total = result.failed ? 0 : result_total_lines(result);
    std::vector<Span> out;
    out.reserve(preview.size() + 1);

    auto preview_color = nxt::Rgba8{tool_palette().fg_subtle};
    for (auto & line : preview)
        out.push_back(span(std::move(line), fg(preview_color)));

    if (preview.size() < total && !preview.empty()) {
        auto more = total - preview.size();
        out.push_back(span(
            std::format("...{} more lines.", more),
            fg(nxt::Rgba8{tool_palette().fg_subtle}) | faint));
    }

    return out;
}

// ============================================================================
// Approval flow
// ============================================================================

enum class ApprovalDecision {
    yes,
    no,
    all,
    deny_all,
    cancel,
};

template<typename ToolSet>
inline nxt::task<ApprovalDecision> request_approval(
    nxt::ui::yard & self,
    const ToolSet & tool_list,
    const tools::function_call & call)
{
    auto args_short = args_summary(tool_list, call);
    auto display = display_for_call(tool_list, call);
    self.draw(approval_card(display, args_short));
    while (!self.cancelled()) {
        auto event = co_await self.next_input();
        if (!event)
            co_return ApprovalDecision::cancel;
        if (event->type == nxt::input::EventType::release)
            continue;
        if (nxt::ui::is_escape(*event))
            co_return ApprovalDecision::cancel;
        if (nxt::ui::is_character(*event, 'y'))
            co_return ApprovalDecision::yes;
        if (nxt::ui::is_character(*event, 'n'))
            co_return ApprovalDecision::no;
        if (nxt::ui::is_character(*event, 'A'))
            co_return ApprovalDecision::all;
        if (nxt::ui::is_character(*event, 'D'))
            co_return ApprovalDecision::deny_all;
    }
    co_return ApprovalDecision::cancel;
}

// ============================================================================
// Dynamic column for concurrent tool surfaces. Same shape as the
// helper in cgroup_browser / span_browser. Owned-vector form so the
// published layout is self-contained.
// ============================================================================

template<nxt::tui::Layout L>
struct DynColumn
{
    std::vector<L> children;

    nxt::tui::WidthHint width_hint() const
    {
        return nxt::tui::WidthHint::grow();
    }

    nxt::tui::HeightHint height_hint() const
    {
        nxt::height_t total{0 * nxt::ln};
        for (const auto & c : children)
            total = total + c.height_hint().min;
        return nxt::tui::HeightHint::fixed(total);
    }

    void render(nxt::RasterView & raster, nxt::Size size) const
    {
        nxt::Pos cursor = nxt::Pos::origin();
        for (const auto & c : children) {
            auto h = c.height_hint().min;
            if (h.count() == 0)
                continue;
            if ((cursor.y - nxt::Pos::origin().y) + h > size.h)
                break;
            auto child_size = nxt::Size{size.w, h};
            auto sub = nxt::tui::subraster(raster, cursor, child_size);
            c.render(sub, child_size);
            cursor = cursor + h;
        }
    }
};

template<nxt::tui::Layout L>
auto dyn_column(std::vector<L> children)
{
    return DynColumn<L>{std::move(children)};
}

template<typename L>
    requires nxt::tui::Layout<std::decay_t<L>>
std::string render_for_scrollback(nxt::ui::yard & self, L && layout)
{
    auto height = layout.height_hint().min;
    if (height.count() == 0)
        height = 1 * nxt::ln;

    nxt::Raster raster(
        self.runtime().terminal_width(), height, self.runtime().glyphs());
    auto view = raster.view();
    layout.render(view, raster.extent());
    return nxt::ansi::render_raster(raster);
}

template<typename ToolSet>
inline void commit_tool_result(
    nxt::ui::yard & self,
    const ToolSet &,
    const tools::function_call &,
    tool_display display,
    std::string_view args_short,
    const tools::tool_result & result,
    std::chrono::milliseconds elapsed,
    hud_blocks::State * hud = nullptr)
{
    using namespace nxt::tui;
    auto summary = result_summary(result);
    auto preview = preview_spans(result, 4);
    auto is_error = result.failed;
    self.print_block(render_for_scrollback(
        self,
        column(
            done_card(display, args_short, elapsed, summary, is_error),
            list(preview, [](const Span & line) {
                return styled_text(line);
            }))));

    auto folded = folded_result_card(
        display, args_short, elapsed, summary, is_error);
    if (hud) {
        hud->add(folded);
        self.draw(hud->view());
    } else {
        self.draw(folded);
    }
}

// ============================================================================
// Per-tool execution (already-decided): animated card + commit.
// Returns the typed result produced by the generic tool runner.
// ============================================================================

template<typename ToolSet>
inline nxt::task<tools::tool_result> run_one_animated(
    nxt::ui::yard & self,
    const ToolSet & tool_list,
    tools::function_call call,
    hud_blocks::State * hud = nullptr)
{
    using namespace nxt::tui;
    auto args_short = args_summary(tool_list, call);
    auto display = display_for_call(tool_list, call);
    auto start = std::chrono::steady_clock::now();

    agent_trace::record_tool_call(self, call);

    tools::tool_result result;
    auto worker = [&result, tool_list_ptr = &tool_list, call](
                      nxt::ui::yard & s) -> nxt::task<> {
        (void) s;
        result = co_await tools::run_function_tool(*tool_list_ptr, call);
    };

    {
        auto companion = [display,
                          args_short,
                          start](nxt::ui::yard & s) -> nxt::task<> {
            int tick = 0;

            while (!s.cancelled()) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start);
                s.draw(running_card(display, args_short, tick, elapsed));
                ++tick;
                co_await s.sleep(40ms);
            }

            co_return;
        };

        co_await nxt::ui::accompany(
            self,
            worker,
            companion,
            [](const auto & comp_surface, const auto & worker_surface) {
                (void) worker_surface;
                return comp_surface;
            });
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    agent_trace::record_tool_result(
        self,
        call,
        tools::tool_result_json(result),
        result.failed);

    commit_tool_result(
        self,
        tool_list,
        call,
        display,
        args_short,
        result,
        elapsed,
        hud);
    co_return result;
}

template<typename ToolSet>
inline nxt::task<tools::tool_result> run_one_or_deny(
    nxt::ui::yard & self,
    const ToolSet & tool_list,
    tools::function_call call,
    bool approved,
    hud_blocks::State * hud = nullptr)
{
    using namespace nxt::tui;
    if (!approved) {
        auto args_short = args_summary(tool_list, call);
        auto display = display_for_call(tool_list, call);
        auto denied = denied_card(display, args_short);
        auto block = render_for_scrollback(self, denied);
        block += "\n";
        self.print_block(block);
        if (hud) {
            hud->add(denied);
            self.draw(denied);
        } else {
            self.draw(nxt::tui::AnyLayout{});
        }
        agent_trace::record_tool_call(self, call);
        auto denial = tools::tool_result{
            .failed = true,
            .output = "denied by user",
        };
        agent_trace::record_tool_result(
            self,
            call,
            tools::tool_result_json(denial),
            true);
        co_return denial;
    }
    co_return co_await run_one_animated(
        self, tool_list, std::move(call), hud);
}

// ============================================================================
// Entry point: collect any required approvals, then spawn all tools
// concurrently, compose their surfaces in a column, await all.
// ============================================================================

template<typename ToolSet>
inline nxt::task<std::vector<openai::raw_json>> run_all(
    nxt::ui::yard & self,
    const ToolSet & tool_list,
    const std::vector<tools::function_call> & calls,
    hud_blocks::State * hud = nullptr)
{
    using namespace nxt::tui;
    if (calls.empty())
        co_return std::vector<openai::raw_json>{};

    // Phase 1: collect approvals sequentially. While we're prompting,
    // the HUD shows the approval card for the current call.
    std::vector<bool> approved(calls.size(), true);
    bool blanket_yes = false;
    bool blanket_no = false;
    bool cancelled = false;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (!display_for_call(tool_list, calls[i]).needs_approval)
            continue;
        if (blanket_yes) {
            approved[i] = true;
            continue;
        }
        if (blanket_no || cancelled) {
            approved[i] = false;
            continue;
        }
        auto d = co_await request_approval(self, tool_list, calls[i]);
        switch (d) {
        case ApprovalDecision::yes:
            approved[i] = true;
            break;
        case ApprovalDecision::no:
            approved[i] = false;
            break;
        case ApprovalDecision::all:
            approved[i] = true;
            blanket_yes = true;
            break;
        case ApprovalDecision::deny_all:
            approved[i] = false;
            blanket_no = true;
            break;
        case ApprovalDecision::cancel:
            approved[i] = false;
            cancelled = true;
            break;
        }
    }

    if (hud)
        self.draw(hud->view());
    else
        self.draw(nxt::tui::AnyLayout{});

    // Phase 2: spawn each call as its own child process. Each child
    // animates its card via `accompany`, runs (or denies), commits
    // to scrollback. Results land in a shared vector indexed by
    // slot — single scheduler thread so no data race.
    auto results =
        std::make_shared<std::vector<tools::tool_result>>(calls.size());

    std::vector<nxt::ui::ProcessHandle> handles;
    handles.reserve(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        auto call_copy = calls[i];
        auto approved_i = approved[i];
        handles.push_back(self.spawn(
            [results,
             i,
             call_copy = std::move(call_copy),
             approved_i,
             hud,
             tool_list_ptr = &tool_list](nxt::ui::yard & s) -> nxt::task<> {
                auto r = co_await run_one_or_deny(
                    s, *tool_list_ptr, call_copy, approved_i, hud);
                (*results)[i] = std::move(r);
            }));
    }

    // Compose surfaces into a column for the duration of the batch.
    std::vector<nxt::tui::AnyLayout> surfaces;
    surfaces.reserve(handles.size());
    for (const auto & h : handles)
        surfaces.emplace_back(h.surface());
    auto active_tools = dyn_column(std::move(surfaces));
    if (hud)
        self.draw(hud->view(std::move(active_tools)));
    else
        self.draw(std::move(active_tools));

    // Wait for all children to finish (each commits its own card to
    // scrollback as it completes; the HUD column shrinks as cards
    // self-erase).
    co_await self.scope().all();

    if (hud)
        self.draw(hud->view());
    else
        self.draw(nxt::tui::AnyLayout{});

    // Serialize each child's typed result as a function_call_output for
    // the LLM's next turn.
    std::vector<openai::raw_json> out;
    out.reserve(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i)
        out.push_back(
            tools::function_call_output(
                calls[i].call_id,
                tools::tool_result_json(
                    std::move((*results)[i]))));
    co_return out;
}

} // namespace nxt::ai::tool_ui
