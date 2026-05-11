#include <nxt/text_field.hpp>
#include <nxt/ansi.hpp>
#include <nxt/tui.hpp>
#include <nxtio/app.hpp>
#include <nxtio/async.hpp>
#include <nxtio/event-queue.hpp>
#include <nxtio/input.hpp>
#include <nxtio/llm.hpp>
#include <nxtio/net.hpp>
#include <nxtio/nxtllm-trace-store.hpp>
#include <nxtio/text_field.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using llm_request = nxt::io::llm::openai_responses_request;
using stream_event = nxt::io::llm::stream_event;
using trace_row = nxt::io::nxtllm::trace_row;

constexpr std::size_t default_max_output_tokens = 128000;

class parquet_trace
{
public:
    explicit parquet_trace(std::optional<std::string> output_path)
        : output_path_(std::move(output_path))
        , start_(std::chrono::steady_clock::now())
    {
        if (output_path_) {
            auto now =
                std::chrono::system_clock::now().time_since_epoch().count();
            run_id_ = "nxtllm-" + std::to_string(now);
        }
    }

    [[nodiscard]] bool enabled() const noexcept
    {
        return output_path_.has_value();
    }

    [[nodiscard]] const std::optional<std::string> & output_path() const
    {
        return output_path_;
    }

    void record_request(const llm_request & request)
    {
        if (!enabled())
            return;

        auto body = nxt::io::llm::openai_responses_body(request);
        auto metadata = nlohmann::json{
            {"provider", "openai"},
            {"api", "responses"},
            {"url", "https://api.openai.com/v1/responses"},
            {"method", "POST"},
            {"request_body", body},
            {"headers",
             {
                 {"Accept", "text/event-stream"},
                 {"Content-Type", "application/json"},
                 {"User-Agent", "nxtllm/0"},
             }},
            {"authorization_header_present", !request.api_key.empty()},
        };

        add("request",
            "openai.responses.request",
            metadata.dump(),
            body.dump());
    }

    void record_event(const stream_event & event)
    {
        if (!enabled())
            return;

        add("sse_event", event.type, event.raw, event.payload.dump());
    }

    void record_marker(std::string phase, std::string data = {})
    {
        if (!enabled())
            return;

        add(std::move(phase), {}, std::move(data), {});
    }

    void write() const
    {
        if (!enabled())
            return;

        nxt::io::nxtllm::write_trace_parquet(*output_path_, run_id_, rows_);
    }

private:
    void
    add(std::string phase,
        std::string event_type,
        std::string data,
        std::string payload_json)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_);
        rows_.push_back(
            trace_row{
                .seq = static_cast<std::int64_t>(rows_.size()),
                .elapsed_ms = elapsed.count(),
                .phase = std::move(phase),
                .event_type = std::move(event_type),
                .data = std::move(data),
                .payload_json = std::move(payload_json),
            });
    }

    std::optional<std::string> output_path_;
    std::string run_id_;
    std::chrono::steady_clock::time_point start_;
    std::vector<trace_row> rows_;
};

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
        if (arg == "--no-hud") {
            options.hud = false;
            options.hud_explicit = true;
            continue;
        }
        if (arg == "--trace" || arg == "--trace-parquet") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--trace requires a parquet output path"};
            options.trace_path = argv[++i];
            continue;
        }
        if (arg == "--playback") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--playback requires a parquet trace path"};
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
                   "  --trace out.parquet\n"
                   "  --playback in.parquet\n"
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
    bool done = false;
    bool busy = false;
    bool input_enabled = false;
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
            text("  " + bytes, muted)),
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

struct hud_stream_connected
{};

struct hud_stream_complete
{};

struct hud_stream_error
{
    std::string message;
};

using hud_stream_item = std::variant<
    hud_stream_connected,
    stream_event,
    hud_stream_complete,
    hud_stream_error>;

struct stream_queue_closed : std::exception
{
    [[nodiscard]] const char * what() const noexcept override
    {
        return "stream event queue closed";
    }
};

nxt::task<> playback_delay(
    nxt::ui::UIRuntime & runtime,
    std::int64_t & previous_elapsed_ms,
    const trace_row & row,
    double playback_speed);

nxt::task<> publish_or_stop(
    nxt::io::event_queue<hud_stream_item> & events, hud_stream_item item)
{
    if (!co_await events.publish(std::move(item)))
        throw stream_queue_closed{};
}

nxt::task<> produce_hud_stream(
    std::unique_ptr<nxt::io_scheduler> & scheduler,
    llm_request request,
    nxt::io::event_queue<hud_stream_item> & events)
{
    auto error = std::optional<std::string>{};
    try {
        auto transport = co_await nxt::io::net::connect_tls(
            scheduler,
            nxt::io::net::endpoint{
                .host = "api.openai.com",
                .port = 443,
            });

        co_await publish_or_stop(events, hud_stream_connected{});

        auto on_event = [&](stream_event event) -> nxt::task<> {
            co_await publish_or_stop(events, std::move(event));
        };

        co_await nxt::io::llm::stream_openai_responses_over(
            transport, request, on_event, events.stop_token());
        co_await transport.shutdown();
        co_await publish_or_stop(events, hud_stream_complete{});
    } catch (const stream_queue_closed &) {
    } catch (const std::exception & e) {
        if (!events.stop_requested())
            error = e.what();
    }

    if (error)
        co_await events.publish(hud_stream_error{std::move(*error)});
    co_await events.close();
}

nxt::task<> produce_playback_stream(
    nxt::ui::UIRuntime & runtime,
    std::vector<trace_row> rows,
    std::size_t start_index,
    double playback_speed,
    nxt::io::event_queue<hud_stream_item> & events)
{
    try {
        co_await publish_or_stop(events, hud_stream_connected{});

        if (rows.empty()) {
            co_await publish_or_stop(events, hud_stream_complete{});
            co_await events.close();
            co_return;
        }

        start_index = std::min(start_index, rows.size() - 1);
        auto previous_elapsed_ms = rows[start_index].elapsed_ms;
        for (auto i = start_index; i < rows.size(); ++i) {
            if (runtime.shutdown_requested() || events.stop_requested())
                break;

            const auto & row = rows[i];
            co_await playback_delay(
                runtime, previous_elapsed_ms, row, playback_speed);

            if (auto event = event_from_trace_row(row)) {
                co_await publish_or_stop(events, std::move(*event));
            } else if (row.phase == "error") {
                co_await publish_or_stop(events, hud_stream_error{row.data});
                co_await events.close();
                co_return;
            } else if (row.phase == "complete") {
                co_await publish_or_stop(events, hud_stream_complete{});
                co_await events.close();
                co_return;
            }
        }

        co_await publish_or_stop(events, hud_stream_complete{});
    } catch (const stream_queue_closed &) {
    }

    co_await events.close();
}

class hud_stream_reader
{
public:
    hud_stream_reader(
        nxt::ui::UIRuntime & runtime,
        llm_hud_state & state,
        parquet_trace & trace,
        nxt::io::event_queue<hud_stream_item> & events)
        : runtime_(runtime)
        , state_(state)
        , trace_(trace)
        , events_(events)
    {
    }

    nxt::task<std::optional<stream_event>> next_event()
    {
        while (!runtime_.shutdown_requested()) {
            auto item = co_await events_.next();
            if (!item)
                co_return std::nullopt;

            if (std::holds_alternative<hud_stream_connected>(*item)) {
                update_hud(runtime_, state_, [](llm_hud_state & hud) {
                    hud.status = "streaming";
                });
                continue;
            }

            if (auto * error = std::get_if<hud_stream_error>(&*item))
                throw std::runtime_error{error->message};

            if (std::holds_alternative<hud_stream_complete>(*item))
                co_return std::nullopt;

            auto event = std::move(std::get<stream_event>(*item));
            trace_.record_event(event);
            update_hud(runtime_, state_, [](llm_hud_state & hud) {
                ++hud.event_count;
            });
            co_return event;
        }

        co_return std::nullopt;
    }

private:
    nxt::ui::UIRuntime & runtime_;
    llm_hud_state & state_;
    parquet_trace & trace_;
    nxt::io::event_queue<hud_stream_item> & events_;
};

bool is_event(const stream_event & event, std::string_view type)
{
    return event.type == type;
}

std::string output_item_type(const stream_event & event)
{
    if (auto it = event.payload.find("item"); it != event.payload.end())
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

nxt::task<> read_text_delta_item(
    hud_stream_reader & reader,
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

    while (auto event = co_await reader.next_event()) {
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
            for_complete_words(text, true, print_segment);
            if (wrote && cursor != 0)
                runtime.print("\n");
            co_return;
        }
    }

    for_complete_words(text, true, print_segment);
    if (wrote && cursor != 0)
        runtime.print("\n");
}

nxt::task<> read_reasoning_item(
    hud_stream_reader & reader,
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state)
{
    co_await read_text_delta_item(
        reader,
        runtime,
        state,
        "response.reasoning_summary_text.delta",
        true,
        [](llm_hud_state & hud, std::string_view) {
            hud.status = "thinking";
        });
}

nxt::task<> read_message_item(
    hud_stream_reader & reader,
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state)
{
    co_await read_text_delta_item(
        reader,
        runtime,
        state,
        "response.output_text.delta",
        false,
        [](llm_hud_state & hud, std::string_view delta) {
            hud.status = "streaming";
            hud.output_bytes += delta.size();
        });
}

nxt::task<> read_output_item(
    hud_stream_reader & reader,
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    const stream_event & first)
{
    auto type = output_item_type(first);

    if (type == "reasoning") {
        co_await read_reasoning_item(reader, runtime, state);
        co_return;
    }

    if (type == "message") {
        co_await read_message_item(reader, runtime, state);
        co_return;
    }

    while (auto event = co_await reader.next_event()) {
        if (is_event(*event, "response.output_item.done"))
            break;
    }

    co_return;
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

nxt::task<> consume_response_stream(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    parquet_trace & trace,
    nxt::io::event_queue<hud_stream_item> & events)
{
    auto reader = hud_stream_reader{runtime, state, trace, events};
    auto stream_error = std::optional<std::string>{};

    try {
        while (auto event = co_await reader.next_event()) {
            if (is_event(*event, "response.output_item.added")) {
                co_await read_output_item(reader, runtime, state, *event);
                continue;
            }

            if (is_event(*event, "response.completed")) {
                trace.record_marker("complete");
                update_hud(runtime, state, [](llm_hud_state & hud) {
                    hud.status = "completed";
                    hud.done = true;
                    hud.busy = false;
                });
                co_return;
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
                co_return;
            }
        }

        if (!runtime.shutdown_requested()) {
            trace.record_marker("complete");
            update_hud(runtime, state, [](llm_hud_state & hud) {
                hud.status = "completed";
                hud.done = true;
                hud.busy = false;
            });
            co_return;
        }
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
        co_return;
    }

    co_await events.cancel();

    trace.record_marker("cancelled");
    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "cancelled";
        hud.done = true;
        hud.busy = false;
    });
}

nxt::task<> stream_live_request(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    parquet_trace & trace)
{
    start_hud_transcript(runtime, state);
    trace.record_request(state.request);
    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "connecting";
    });

    auto events = nxt::io::event_queue<hud_stream_item>{};
    auto producer = runtime.scheduler().spawn_joinable(produce_hud_stream(
        runtime.scheduler_handle(), state.request, events));

    co_await consume_response_stream(runtime, state, trace, events);
    co_await events.cancel();
    co_await producer;
}

nxt::task<> stream_playback_request(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    parquet_trace & trace,
    std::vector<trace_row> rows,
    std::size_t start_index,
    double playback_speed)
{
    start_hud_transcript(runtime, state);
    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "replaying";
    });

    auto events = nxt::io::event_queue<hud_stream_item>{};
    auto producer = runtime.scheduler().spawn_joinable(produce_playback_stream(
        runtime, std::move(rows), start_index, playback_speed, events));

    co_await consume_response_stream(runtime, state, trace, events);
    co_await events.cancel();
    co_await producer;
}

nxt::task<> run_hud(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    parquet_trace & trace)
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
    parquet_trace & trace)
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
    parquet_trace & trace,
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
    auto trace = parquet_trace{std::nullopt};
    try {
        auto options = parse_args(argc, argv);
        if (options.playback_path && options.trace_path)
            throw std::runtime_error{
                "--trace records live requests; use --playback without it"};

        if (options.playback_path) {
            auto rows =
                nxt::io::nxtllm::read_trace_parquet(*options.playback_path);
            auto start_index =
                playback_start_index(rows, options.playback_from);
            auto state = llm_hud_state{};
            state.request = request_from_trace(rows, options);
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

        trace = parquet_trace{options.trace_path};

        auto state = llm_hud_state{};
        state.request = make_request(options, {});
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
            std::cout << "wrote trace parquet: " << *trace.output_path()
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
