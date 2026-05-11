#include <nxt/text_field.hpp>
#include <nxt/ansi.hpp>
#include <nxt/tui.hpp>
#include <nxtio/app.hpp>
#include <nxtio/async.hpp>
#include <nxtio/input.hpp>
#include <nxtio/llm-trace.hpp>
#include <nxtio/llm.hpp>
#include <nxtio/net.hpp>
#include <nxtio/text_field.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using llm_request = nxt::io::llm::openai_responses_request;
using arrow_trace = nxt::io::nxtllm::arrow_trace;
using function_call = nxt::io::llm::function_call;
using function_tool = nxt::io::llm::function_tool;
using stream_event = nxt::io::llm::stream_event;
using trace_row = nxt::io::nxtllm::trace_row;

constexpr std::size_t default_max_output_tokens = 128000;

struct cli_options
{
    std::string input;
    std::string model = "gpt-5.5";
    std::size_t max_output_tokens = default_max_output_tokens;
    std::string reasoning_effort = "medium";
    std::string reasoning_summary = "auto";
    std::optional<std::string> trace_path;
    std::optional<std::string> playback_path;
    std::optional<std::string> playback_from;
    bool agent = false;
    bool hud = true;
    bool hud_explicit = false;
    bool input_provided = false;
    double playback_speed = 0.0;
};

cli_options parse_args(int argc, char ** argv)
{
    auto options = cli_options{};
    auto positionals = std::vector<std::string>{};

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--hud") {
            options.hud = true;
            options.hud_explicit = true;
            continue;
        }
        if (arg == "--agent") {
            options.agent = true;
            continue;
        }
        if (arg == "--no-agent") {
            options.agent = false;
            continue;
        }
        if (arg == "--no-hud") {
            options.hud = false;
            options.hud_explicit = true;
            continue;
        }
        if (arg == "--trace" || arg == "--trace-arrow") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--trace requires an Arrow IPC output path"};
            options.trace_path = argv[++i];
            continue;
        }
        if (arg == "--playback") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--playback requires an Arrow IPC trace path"};
            options.playback_path = argv[++i];
            continue;
        }
        if (arg == "--playback-realtime") {
            options.playback_speed = 1.0;
            continue;
        }
        if (arg == "--playback-from") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--playback-from requires a value like 70%"};
            options.playback_from = argv[++i];
            continue;
        }
        if (arg == "--playback-speed") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--playback-speed requires a value"};
            options.playback_speed = std::stod(argv[++i]);
            if (options.playback_speed < 0.0)
                throw std::runtime_error{
                    "--playback-speed must be non-negative"};
            continue;
        }
        if (arg == "--max-output-tokens") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--max-output-tokens requires a value"};
            options.max_output_tokens = std::stoull(argv[++i]);
            continue;
        }
        if (arg == "--reasoning-effort") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--reasoning-effort requires a value"};
            options.reasoning_effort = argv[++i];
            continue;
        }
        if (arg == "--reasoning-summary") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--reasoning-summary requires a value"};
            options.reasoning_summary = argv[++i];
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: nxtllm [options] [input] [model]\n"
                   "  --hud                    open the prompt HUD\n"
                   "  --no-hud                 run one prompt in plain CLI mode\n"
                   "  --agent                  enable local function tools\n"
                   "  --no-agent               disable local function tools\n"
                   "  --trace out.arrow\n"
                   "  --playback in.arrow\n"
                   "  --playback-realtime\n"
                   "  --playback-from 70%     seek by trace elapsed time\n"
                   "  --playback-speed N        0 means as fast as possible\n"
                   "  --max-output-tokens N\n"
                   "  --reasoning-effort minimal|low|medium|high|xhigh\n"
                   "  --reasoning-summary auto|concise|detailed|none\n";
            std::exit(EXIT_SUCCESS);
        }
        positionals.emplace_back(arg);
    }

    if (!positionals.empty()) {
        options.input = std::move(positionals[0]);
        options.input_provided = true;
    }
    if (positionals.size() > 1)
        options.model = std::move(positionals[1]);
    if (positionals.size() > 2)
        throw std::runtime_error{"too many positional arguments"};

    if (options.input_provided && !options.hud_explicit)
        options.hud = false;

    return options;
}

llm_request make_request(const cli_options & options, std::string api_key)
{
    return llm_request{
        .api_key = std::move(api_key),
        .model = options.model,
        .input = options.input,
        .input_items = nlohmann::json::array(),
        .tools = nlohmann::json::array(),
        .previous_response_id = {},
        .max_output_tokens = options.max_output_tokens,
        .reasoning_effort = options.reasoning_effort,
        .reasoning_summary = options.reasoning_summary == "none"
                                 ? std::string{}
                                 : options.reasoning_summary,
    };
}

std::size_t playback_start_index(
    const std::vector<trace_row> & rows,
    const std::optional<std::string> & playback_from)
{
    if (!playback_from || rows.empty())
        return 0;

    const auto & text = *playback_from;
    std::size_t parsed = 0;
    auto percent = std::stod(text, &parsed);
    if (parsed + 1 != text.size() || text[parsed] != '%')
        throw std::runtime_error{
            "--playback-from currently expects a percentage like 70%"};
    if (percent < 0.0 || percent > 100.0)
        throw std::runtime_error{
            "--playback-from percentage must be between 0% and 100%"};

    auto first_elapsed = rows.front().elapsed_ms;
    auto last_elapsed = rows.back().elapsed_ms;
    auto span = std::max<std::int64_t>(0, last_elapsed - first_elapsed);
    auto target = first_elapsed
                  + static_cast<std::int64_t>(
                      static_cast<double>(span) * percent / 100.0);

    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].elapsed_ms >= target)
            return i;
    }
    return rows.size() - 1;
}

llm_request request_from_trace(
    const std::vector<trace_row> & rows, const cli_options & options)
{
    auto request = make_request(options, {});
    for (const auto & row : rows) {
        if (row.phase != "request" || row.payload_json.empty())
            continue;

        auto body = nlohmann::json::parse(row.payload_json);
        request.model = body.value("model", request.model);
        request.input = body.value("input", request.input);
        request.max_output_tokens =
            body.value("max_output_tokens", request.max_output_tokens);
        request.store = body.value("store", request.store);
        if (auto it = body.find("reasoning"); it != body.end()) {
            request.reasoning_effort =
                it->value("effort", request.reasoning_effort);
            request.reasoning_summary =
                it->value("summary", request.reasoning_summary);
        }
        return request;
    }
    return request;
}

std::optional<stream_event> event_from_trace_row(const trace_row & row)
{
    if (row.phase != "sse_event")
        return std::nullopt;

    auto payload_text =
        row.payload_json.empty() ? row.data : row.payload_json;
    auto payload = nlohmann::json::parse(payload_text);
    auto type = row.event_type.empty()
                    ? payload.value("type", std::string{})
                    : row.event_type;
    return stream_event{
        .type = std::move(type),
        .payload = std::move(payload),
        .raw = row.data,
    };
}

struct llm_hud_state
{
    llm_request request;
    nxt::tui::TextField input;
    std::string status = "ready";
    std::string error;
    std::size_t event_count = 0;
    std::size_t output_bytes = 0;
    std::size_t tool_call_count = 0;
    bool done = false;
    bool busy = false;
    bool input_enabled = false;
    bool agent_enabled = false;
};

std::string spinner_for(const llm_hud_state & state)
{
    if (!state.busy && !state.done)
        return ">";
    if (state.done)
        return state.error.empty() ? "ok" : "!!";

    constexpr auto frames = std::array{"-", "\\", "|", "/"};
    return frames[state.event_count % frames.size()];
}

void reset_hud_request_state(llm_hud_state & state)
{
    state.status = "ready";
    state.error.clear();
    state.event_count = 0;
    state.output_bytes = 0;
    state.tool_call_count = 0;
    state.done = false;
}

void update_hud(
    nxt::ui::UIRuntime & runtime, llm_hud_state & state, auto fn)
{
    fn(state);
    runtime.signal_damage();
}

auto build_hud(const llm_hud_state & state)
{
    using namespace nxt::tui;

    auto muted = fg(nxt::Rgba8{150, 156, 162});
    auto normal = fg(nxt::Rgba8{220, 224, 228});
    auto good = fg(nxt::Rgba8{125, 200, 145}) | bold;
    auto bad = fg(nxt::Rgba8{235, 120, 120}) | bold;
    auto status_style =
        state.error.empty() ? (!state.busy ? good : normal) : bad;

    auto status = spinner_for(state) + " " + state.status;
    auto events = "events " + std::to_string(state.event_count);
    auto bytes = "bytes " + std::to_string(state.output_bytes);
    auto tools = " tools " + std::to_string(state.tool_call_count);
    auto field_style = TextFieldStyle{
        .fg = state.busy ? nxt::Rgba8{150, 156, 162}
                         : nxt::Rgba8{220, 224, 228},
        .bg = nxt::Rgba8{28, 32, 36},
        .prefix_fg = state.busy || !state.input_enabled
                         ? nxt::Rgba8{150, 156, 162}
                         : nxt::Rgba8{90, 190, 210},
        .placeholder_fg = nxt::Rgba8{105, 110, 118},
    };
    auto input = state.input;
    if (!state.input_enabled) {
        input.text = state.request.input;
        input.cursor_byte = nxt::utf8::byte_offset(input.text.size());
    }

    return either(
        !state.busy,
        row(text(status, status_style),
            text("  " + events, muted),
            text("  " + bytes, muted),
            text(state.agent_enabled ? tools : "", muted)),
        text_field(
            input,
            {
                .prefix = state.request.model + "> ",
                .placeholder = "",
                .style = field_style,
                .focused = state.input_enabled && !state.busy,
            }));
}

void start_hud_transcript(
    nxt::ui::UIRuntime & runtime, llm_hud_state & state)
{
    runtime.println("> " + state.request.input);
}

bool load_api_key(nxt::ui::UIRuntime & runtime, llm_hud_state & state)
{
    auto api_key = std::getenv("OPENAI_API_KEY");
    if (api_key != nullptr && !std::string_view{api_key}.empty()) {
        state.request.api_key = api_key;
        return true;
    }

    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "error";
        hud.error = "OPENAI_API_KEY is not set";
        hud.done = true;
        hud.busy = false;
    });
    runtime.println("error: OPENAI_API_KEY is not set");
    return false;
}

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required)
{
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

std::string local_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = std::tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    auto out = std::ostringstream{};
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S %z");
    return out.str();
}

std::vector<function_tool> builtin_agent_tools(nxt::ui::UIRuntime & runtime)
{
    auto tools = std::vector<function_tool>{};

    tools.push_back(function_tool{
        .name = "nxt_current_time",
        .description =
            "Return the current local timestamp for the nxtllm process.",
        .parameters = object_schema(nlohmann::json::object(), nlohmann::json::array()),
        .strict = true,
        .run = [](const nlohmann::json &) -> nxt::task<std::string> {
            co_return nlohmann::json{
                {"local_time", local_timestamp()},
            }.dump();
        },
    });

    tools.push_back(function_tool{
        .name = "nxt_terminal_size",
        .description =
            "Return the current terminal size used by the nxt UI runtime.",
        .parameters = object_schema(nlohmann::json::object(), nlohmann::json::array()),
        .strict = true,
        .run = [&runtime](const nlohmann::json &) -> nxt::task<std::string> {
            co_return nlohmann::json{
                {"columns", runtime.terminal_width().count()},
                {"rows", runtime.terminal_height().count()},
            }.dump();
        },
    });

    tools.push_back(function_tool{
        .name = "nxt_echo",
        .description =
            "Echo a short text string. Useful for checking that tool calling works.",
        .parameters = object_schema(
            {
                {"text",
                 {
                     {"type", "string"},
                     {"description", "Text to echo back."},
                 }},
            },
            {"text"}),
        .strict = true,
        .run = [](const nlohmann::json & args) -> nxt::task<std::string> {
            co_return nlohmann::json{
                {"text", args.value("text", std::string{})},
            }.dump();
        },
    });

    return tools;
}

nxt::task<> playback_delay(
    nxt::ui::UIRuntime & runtime,
    std::int64_t & previous_elapsed_ms,
    const trace_row & row,
    double playback_speed);

// Pull-shaped event source backed by a recorded Arrow IPC trace. Mirrors the
// shape of nxt::io::llm::openai_response_stream so the consumer loop can be
// written once and reused for both live and replay.
class playback_stream
{
public:
    playback_stream(
        nxt::ui::UIRuntime & runtime,
        std::vector<trace_row> rows,
        std::size_t start_index,
        double playback_speed)
        : runtime_(&runtime)
        , rows_(std::move(rows))
        , index_(rows_.empty() ? 0 : std::min(start_index, rows_.size() - 1))
        , previous_elapsed_ms_(rows_.empty() ? 0 : rows_[index_].elapsed_ms)
        , playback_speed_(playback_speed)
    {
    }

    playback_stream(const playback_stream &) = delete;
    playback_stream & operator=(const playback_stream &) = delete;
    playback_stream(playback_stream &&) = delete;
    playback_stream & operator=(playback_stream &&) = delete;

    nxt::task<std::optional<stream_event>> next()
    {
        while (index_ < rows_.size()) {
            if (runtime_->shutdown_requested())
                co_return std::nullopt;

            const auto & row = rows_[index_++];
            co_await playback_delay(
                *runtime_, previous_elapsed_ms_, row, playback_speed_);

            if (row.phase == "error")
                throw std::runtime_error{row.data};
            if (row.phase == "complete")
                co_return std::nullopt;
            if (auto event = event_from_trace_row(row))
                co_return std::move(*event);
        }
        co_return std::nullopt;
    }

private:
    nxt::ui::UIRuntime * runtime_;
    std::vector<trace_row> rows_;
    std::size_t index_;
    std::int64_t previous_elapsed_ms_;
    double playback_speed_;
};

bool is_event(const stream_event & event, std::string_view type)
{
    return event.type == type;
}

struct response_stream_result
{
    std::vector<function_call> function_calls;
    std::vector<nlohmann::json> output_items;
    std::optional<std::string> response_id;
    bool completed = false;
};

struct output_item_result
{
    std::optional<function_call> call;
    std::optional<nlohmann::json> item;
};

std::optional<nlohmann::json> output_item_from_event(const stream_event & event)
{
    if (auto it = event.payload.find("item");
        it != event.payload.end() && it->is_object())
        return *it;
    return std::nullopt;
}

std::string output_item_type(const stream_event & event)
{
    if (auto it = event.payload.find("item");
        it != event.payload.end() && it->is_object())
        return it->value("type", std::string{});
    return {};
}

std::size_t stream_wrap_width(nxt::ui::UIRuntime & runtime)
{
    auto columns = runtime.terminal_width().count();
    if (columns > 32)
        return columns - 8;
    return std::max<std::size_t>(1, columns);
}

void for_complete_words(std::string & text, bool finish, auto fn)
{
    auto n = finish ? text.size() : nxt::utf8::complete_words_prefix_size(text);
    auto complete = std::string_view{text}.substr(0, n);
    for (auto segment : nxt::utf8::segments(complete))
        fn(segment);
    text.erase(0, n);
}

void print_text(
    nxt::ui::UIRuntime & runtime,
    std::string_view text,
    bool dim)
{
    if (!dim) {
        runtime.print(text);
        return;
    }

    auto styled = std::string{};
    auto writer = nxt::ansi::Writer{styled};
    writer.dim();
    writer.text(text);
    writer.reset();
    runtime.print(styled);
}

template<typename Stream>
nxt::task<std::optional<nlohmann::json>> read_text_delta_item(
    Stream & stream,
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    std::string_view delta_event_type,
    bool dim,
    auto update_state)
{
    auto text = std::string{};
    auto cursor = std::size_t{0};
    auto wrap_width = stream_wrap_width(runtime);
    auto wrote = false;

    auto print_segment = [&](nxt::utf8::text_segment segment) {
        if (segment.kind == nxt::utf8::text_segment::kind_t::line_break) {
            runtime.print("\n");
            cursor = 0;
            wrote = true;
            return;
        }

        auto word_width = segment.width.count();
        if (cursor > 0 && cursor + word_width > wrap_width) {
            runtime.print("\n");
            cursor = 0;
        }
        print_text(runtime, segment.text, dim);
        runtime.print(" ");
        cursor += word_width + 1;
        wrote = true;
    };

    while (auto event = co_await stream.next()) {
        if (is_event(*event, delta_event_type)) {
            auto delta = event->payload.value("delta", std::string{});
            if (!delta.empty()) {
                text += delta;
                for_complete_words(text, false, print_segment);
                update_hud(runtime, state, [&](llm_hud_state & hud) {
                    update_state(hud, std::string_view{delta});
                });
            }
            continue;
        }

        if (is_event(*event, "response.output_item.done")) {
            auto item = output_item_from_event(*event);
            for_complete_words(text, true, print_segment);
            if (wrote && cursor != 0)
                runtime.print("\n");
            co_return item;
        }
    }

    for_complete_words(text, true, print_segment);
    if (wrote && cursor != 0)
        runtime.print("\n");
    co_return std::nullopt;
}

template<typename Stream>
nxt::task<std::optional<nlohmann::json>> read_reasoning_item(
    Stream & stream, nxt::ui::UIRuntime & runtime, llm_hud_state & state)
{
    co_return co_await read_text_delta_item(
        stream,
        runtime,
        state,
        "response.reasoning_summary_text.delta",
        true,
        [](llm_hud_state & hud, std::string_view) {
            hud.status = "thinking";
        });
}

template<typename Stream>
nxt::task<std::optional<nlohmann::json>> read_message_item(
    Stream & stream, nxt::ui::UIRuntime & runtime, llm_hud_state & state)
{
    co_return co_await read_text_delta_item(
        stream,
        runtime,
        state,
        "response.output_text.delta",
        false,
        [](llm_hud_state & hud, std::string_view delta) {
            hud.status = "streaming";
            hud.output_bytes += delta.size();
        });
}

template<typename Stream>
nxt::task<output_item_result> read_output_item(
    Stream & stream,
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    const stream_event & first)
{
    auto type = output_item_type(first);

    if (type == "reasoning") {
        co_return output_item_result{
            .call = std::nullopt,
            .item = co_await read_reasoning_item(stream, runtime, state),
        };
    }

    if (type == "message") {
        co_return output_item_result{
            .call = std::nullopt,
            .item = co_await read_message_item(stream, runtime, state),
        };
    }

    if (type == "function_call") {
        auto item = output_item_from_event(first);

        while (auto event = co_await stream.next()) {
            if (is_event(*event, "response.output_item.done")) {
                if (auto done_item = output_item_from_event(*event))
                    item = std::move(*done_item);
                break;
            }
        }

        auto call = item
            ? nxt::io::llm::function_call_from_item(*item)
            : std::optional<function_call>{};
        co_return output_item_result{
            .call = std::move(call),
            .item = std::move(item),
        };
    }

    auto item = output_item_from_event(first);
    while (auto event = co_await stream.next()) {
        if (is_event(*event, "response.output_item.done")) {
            if (auto done_item = output_item_from_event(*event))
                item = std::move(*done_item);
            break;
        }
    }

    co_return output_item_result{
        .call = std::nullopt,
        .item = std::move(item),
    };
}

nxt::task<> playback_delay(
    nxt::ui::UIRuntime & runtime,
    std::int64_t & previous_elapsed_ms,
    const trace_row & row,
    double playback_speed)
{
    if (playback_speed <= 0.0) {
        previous_elapsed_ms = row.elapsed_ms;
        co_return;
    }

    auto delta_ms =
        std::max<std::int64_t>(0, row.elapsed_ms - previous_elapsed_ms);
    previous_elapsed_ms = row.elapsed_ms;
    auto scaled_ms = static_cast<std::int64_t>(
        static_cast<double>(delta_ms) / playback_speed);
    if (scaled_ms > 0)
        co_await runtime.sleep(std::chrono::milliseconds{scaled_ms});
}

// Pass-through stream that records each pulled event into the Arrow IPC trace
// and bumps the HUD event counter.  Wraps any inner stream that exposes
// task<optional<stream_event>> next().
template<typename Inner>
class traced_stream
{
public:
    traced_stream(
        Inner & inner,
        nxt::ui::UIRuntime & runtime,
        llm_hud_state & state,
        arrow_trace & trace)
        : inner_(&inner)
        , runtime_(&runtime)
        , state_(&state)
        , trace_(&trace)
    {
    }

    nxt::task<std::optional<stream_event>> next()
    {
        auto event = co_await inner_->next();
        if (event) {
            trace_->record_event(*event);
            update_hud(*runtime_, *state_, [](llm_hud_state & hud) {
                ++hud.event_count;
            });
        }
        co_return event;
    }

private:
    Inner * inner_;
    nxt::ui::UIRuntime * runtime_;
    llm_hud_state * state_;
    arrow_trace * trace_;
};

template<typename Stream>
nxt::task<response_stream_result> consume_response_stream(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    arrow_trace & trace,
    Stream & stream)
{
    auto stream_error = std::optional<std::string>{};
    auto result = response_stream_result{};

    try {
        while (auto event = co_await stream.next()) {
            if (auto response_id = nxt::io::llm::response_id_from_event(*event))
                result.response_id = std::move(*response_id);

            if (is_event(*event, "response.output_item.added")) {
                auto output_item =
                    co_await read_output_item(stream, runtime, state, *event);
                if (output_item.item)
                    result.output_items.push_back(std::move(*output_item.item));
                if (output_item.call) {
                    result.function_calls.push_back(
                        std::move(*output_item.call));
                    update_hud(runtime, state, [](llm_hud_state & hud) {
                        hud.status = "tool requested";
                        ++hud.tool_call_count;
                    });
                }
                continue;
            }

            if (is_event(*event, "response.completed")) {
                if (auto response_id = nxt::io::llm::response_id_from_event(*event))
                    result.response_id = std::move(*response_id);
                trace.record_marker("complete");
                update_hud(runtime, state, [&](llm_hud_state & hud) {
                    if (result.function_calls.empty()) {
                        hud.status = "completed";
                        hud.done = true;
                        hud.busy = false;
                    } else {
                        hud.status = "running tools";
                        hud.done = false;
                        hud.busy = true;
                    }
                });
                result.completed = true;
                co_return result;
            }

            if (is_event(*event, "response.failed")
                || is_event(*event, "response.incomplete")) {
                trace.record_marker("error", event->type);
                update_hud(runtime, state, [&](llm_hud_state & hud) {
                    hud.status = event->type;
                    hud.error = event->type;
                    hud.done = true;
                    hud.busy = false;
                });
                co_return result;
            }
        }

        if (runtime.shutdown_requested()) {
            trace.record_marker("cancelled");
            update_hud(runtime, state, [](llm_hud_state & hud) {
                hud.status = "cancelled";
                hud.done = true;
                hud.busy = false;
            });
            co_return result;
        }

        trace.record_marker("complete");
        update_hud(runtime, state, [](llm_hud_state & hud) {
            hud.status = "completed";
            hud.done = true;
            hud.busy = false;
        });
        result.completed = true;
        co_return result;
    } catch (const std::exception & e) {
        if (!runtime.shutdown_requested())
            stream_error = e.what();
    }

    if (stream_error) {
        trace.record_marker("error", *stream_error);
        runtime.println("error: " + *stream_error);
        update_hud(runtime, state, [&](llm_hud_state & hud) {
            hud.status = "error";
            hud.error = *stream_error;
            hud.done = true;
            hud.busy = false;
        });
        co_return result;
    }

    trace.record_marker("cancelled");
    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "cancelled";
        hud.done = true;
        hud.busy = false;
    });
    co_return result;
}

nxt::task<response_stream_result> stream_live_response_once(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    arrow_trace & trace,
    const llm_request & request)
{
    trace.record_request(request);
    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "connecting";
    });

    auto transport = co_await nxt::io::net::connect_tls(
        runtime.scheduler_handle(),
        nxt::io::net::endpoint{
            .host = "api.openai.com",
            .port = 443,
        });

    using transport_t = decltype(transport);
    auto stream =
        nxt::io::llm::openai_response_stream<transport_t>{
            transport, runtime.get_stop_token()};

    auto result = response_stream_result{};
    try {
        co_await stream.connect(request);
        update_hud(runtime, state, [](llm_hud_state & hud) {
            hud.status = "streaming";
        });

        auto traced = traced_stream{stream, runtime, state, trace};
        result = co_await consume_response_stream(runtime, state, trace, traced);
    } catch (const std::exception & e) {
        if (!runtime.shutdown_requested()) {
            trace.record_marker("error", e.what());
            runtime.println(std::string{"error: "} + e.what());
            update_hud(runtime, state, [&](llm_hud_state & hud) {
                hud.status = "error";
                hud.error = e.what();
                hud.done = true;
                hud.busy = false;
            });
        }
    }

    co_await transport.shutdown();
    co_return result;
}

nxt::task<std::vector<nlohmann::json>> run_agent_tools(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    arrow_trace & trace,
    const std::vector<function_tool> & tools,
    const std::vector<function_call> & calls)
{
    auto outputs = std::vector<nlohmann::json>{};
    outputs.reserve(calls.size());

    for (const auto & call : calls) {
        trace.record_marker("tool_call", call.name);
        runtime.println(
            std::format("tool: {}({})", call.name, call.arguments));
        update_hud(runtime, state, [&](llm_hud_state & hud) {
            hud.status = "tool " + call.name;
        });

        auto output =
            co_await nxt::io::llm::run_function_tool(tools, call);
        trace.record_marker("tool_output", output);
        runtime.println(std::format("tool result: {}", output));
        outputs.push_back(nxt::io::llm::function_call_output(
            call.call_id,
            std::move(output)));
    }

    co_return outputs;
}

nxt::task<> stream_live_request(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    arrow_trace & trace)
{
    start_hud_transcript(runtime, state);

    auto tools = state.agent_enabled
        ? builtin_agent_tools(runtime)
        : std::vector<function_tool>{};
    auto request = state.request;
    if (!tools.empty())
        request.tools = nxt::io::llm::function_tool_definitions(tools);
    auto stateless_input =
        nxt::io::llm::input_items_from_request(request);

    constexpr auto max_agent_steps = 6;
    for (int step = 0; step < max_agent_steps; ++step) {
        auto result =
            co_await stream_live_response_once(runtime, state, trace, request);

        if (runtime.shutdown_requested() || state.error.size() > 0)
            co_return;
        if (result.function_calls.empty())
            co_return;

        if (!state.agent_enabled) {
            runtime.println("model requested a tool, but --agent is disabled");
            update_hud(runtime, state, [](llm_hud_state & hud) {
                hud.status = "tool call blocked";
                hud.done = true;
                hud.busy = false;
            });
            co_return;
        }

        if (request.store && !result.response_id) {
            runtime.println("error: tool call response had no response id");
            update_hud(runtime, state, [](llm_hud_state & hud) {
                hud.status = "error";
                hud.error = "tool call response had no response id";
                hud.done = true;
                hud.busy = false;
            });
            co_return;
        }

        auto outputs = co_await run_agent_tools(
            runtime,
            state,
            trace,
            tools,
            result.function_calls);

        request = state.request;
        request.input.clear();
        request.previous_response_id.clear();
        if (request.store) {
            request.input_items = std::move(outputs);
            request.previous_response_id = *result.response_id;
        } else {
            for (auto & item : result.output_items)
                stateless_input.push_back(std::move(item));
            for (auto & output : outputs)
                stateless_input.push_back(std::move(output));
            request.input_items = stateless_input;
        }
        request.tools = nxt::io::llm::function_tool_definitions(tools);
        update_hud(runtime, state, [](llm_hud_state & hud) {
            hud.status = "continuing";
            hud.busy = true;
            hud.done = false;
        });
    }

    runtime.println("error: agent step limit reached");
    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "error";
        hud.error = "agent step limit reached";
        hud.done = true;
        hud.busy = false;
    });
}

nxt::task<> stream_playback_request(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    arrow_trace & trace,
    std::vector<trace_row> rows,
    std::size_t start_index,
    double playback_speed)
{
    start_hud_transcript(runtime, state);
    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "replaying";
    });

    auto stream = playback_stream{
        runtime, std::move(rows), start_index, playback_speed};
    auto traced = traced_stream{stream, runtime, state, trace};
    co_await consume_response_stream(runtime, state, trace, traced);
}

nxt::task<> run_hud(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    arrow_trace & trace)
{
    while (!runtime.shutdown_requested()) {
        auto event = co_await runtime.next_input();
        if (!event)
            co_return;

        if (event->type == nxt::input::EventType::release)
            continue;

        if (event->key == nxt::input::Key::escape) {
            runtime.request_shutdown();
            co_return;
        }

        if (state.busy)
            continue;

        if (event->key == nxt::input::Key::enter) {
            if (state.input.empty())
                continue;

            auto prompt = std::move(state.input.text);
            state.input.clear();
            reset_hud_request_state(state);
            state.busy = true;
            state.request.input = std::move(prompt);

            if (!load_api_key(runtime, state))
                continue;

            runtime.println("");
            runtime.signal_damage();
            co_await stream_live_request(runtime, state, trace);
            continue;
        }

        if (nxt::tui::apply_key(state.input, *event))
            runtime.signal_damage();
    }
}

nxt::task<> run_single_prompt(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    arrow_trace & trace)
{
    reset_hud_request_state(state);
    state.busy = true;
    if (load_api_key(runtime, state))
        co_await stream_live_request(runtime, state, trace);
    runtime.request_shutdown();
}

nxt::task<> run_playback(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    arrow_trace & trace,
    std::vector<trace_row> rows,
    std::size_t start_index,
    double playback_speed)
{
    reset_hud_request_state(state);
    state.busy = true;
    co_await stream_playback_request(
        runtime,
        state,
        trace,
        std::move(rows),
        start_index,
        playback_speed);
    runtime.request_shutdown();
}

} // namespace

int main(int argc, char ** argv)
{
    auto trace = arrow_trace{std::nullopt};
    try {
        auto options = parse_args(argc, argv);
        if (options.playback_path && options.trace_path)
            throw std::runtime_error{
                "--trace records live requests; use --playback without it"};

        if (options.playback_path) {
            auto rows =
                nxt::io::nxtllm::read_trace_ipc(*options.playback_path);
            auto start_index =
                playback_start_index(rows, options.playback_from);
            auto state = llm_hud_state{};
            state.request = request_from_trace(rows, options);
            state.agent_enabled = options.agent;
            auto update =
                [&trace,
                 rows = std::move(rows),
                 speed = options.playback_speed,
                 start_index](
                    nxt::ui::UIRuntime & runtime,
                    llm_hud_state & hud) mutable -> nxt::task<> {
                co_await run_playback(
                    runtime,
                    hud,
                    trace,
                    std::move(rows),
                    start_index,
                    speed);
            };

            if (options.hud)
                return nxt::ui::run(std::move(state), build_hud, update);
            return nxt::ui::run_headless(std::move(state), update);
        }

        trace = arrow_trace{options.trace_path};

        auto state = llm_hud_state{};
        state.request = make_request(options, {});
        state.agent_enabled = options.agent;
        auto status = 0;
        if (options.hud) {
            state.input_enabled = true;
            if (options.input_provided) {
                state.input.text = state.request.input;
                state.input.cursor_byte =
                    nxt::utf8::byte_offset(state.input.text.size());
            } else {
                state.request.input.clear();
            }
            status = nxt::ui::run(
                std::move(state),
                build_hud,
                [&trace](nxt::ui::UIRuntime & runtime, llm_hud_state & hud)
                    -> nxt::task<> {
                    co_await run_hud(runtime, hud, trace);
                });
        } else {
            status = nxt::ui::run_headless(
                std::move(state),
                [&trace](nxt::ui::UIRuntime & runtime, llm_hud_state & hud)
                    -> nxt::task<> {
                    co_await run_single_prompt(runtime, hud, trace);
                });
        }
        trace.write();
        if (trace.output_path())
            std::cout << "wrote trace Arrow IPC: " << *trace.output_path()
                      << '\n';
        return status;

    } catch (const std::exception & e) {
        try {
            trace.write();
        } catch (const std::exception & write_error) {
            std::cerr << "nxtllm trace write error: " << write_error.what()
                      << '\n';
        }
        std::cerr << "nxtllm error: " << e.what() << '\n';
        return 1;
    }
}
