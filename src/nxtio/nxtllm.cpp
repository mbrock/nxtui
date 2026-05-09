#include <nxt/tui.hpp>
#include <nxtio/app.hpp>
#include <nxtio/async.hpp>
#include <nxtio/llm.hpp>
#include <nxtio/net.hpp>

#include <duckdb.hpp>
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
#include <vector>

namespace {

using llm_request = nxt::io::llm::openai_responses_request;
using stream_event = nxt::io::llm::stream_event;

struct trace_row
{
    std::int64_t seq = 0;
    std::int64_t elapsed_ms = 0;
    std::string phase;
    std::string event_type;
    std::string data;
    std::string payload_json;
};

std::string sql_string(std::string_view text)
{
    auto out = std::string{};
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(c);
        if (c == '\'')
            out.push_back('\'');
    }
    return out;
}

void checked_query(duckdb::Connection & connection, const std::string & sql)
{
    auto result = connection.Query(sql);
    if (result->HasError())
        throw std::runtime_error{result->GetError()};
}

class parquet_trace
{
public:
    explicit parquet_trace(std::optional<std::string> output_path)
        : output_path_(std::move(output_path))
        , start_(std::chrono::steady_clock::now())
    {
        if (output_path_) {
            auto now = std::chrono::system_clock::now()
                           .time_since_epoch()
                           .count();
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

        add(
            "request",
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

        auto db = duckdb::DuckDB{nullptr};
        auto connection = duckdb::Connection{db};
        checked_query(
            connection,
            "create table trace ("
            "run_id varchar,"
            "seq bigint,"
            "elapsed_ms bigint,"
            "phase varchar,"
            "event_type varchar,"
            "data varchar,"
            "payload_json varchar)");

        auto appender = duckdb::Appender{connection, "trace"};
        for (const auto & row : rows_) {
            appender.BeginRow();
            appender.Append(run_id_.c_str());
            appender.Append(row.seq);
            appender.Append(row.elapsed_ms);
            appender.Append(row.phase.c_str());
            appender.Append(row.event_type.c_str());
            appender.Append(row.data.c_str());
            appender.Append(row.payload_json.c_str());
            appender.EndRow();
        }
        appender.Close();

        checked_query(
            connection,
            "copy trace to '" + sql_string(*output_path_)
                + "' (format parquet)");
    }

private:
    void add(
        std::string phase,
        std::string event_type,
        std::string data,
        std::string payload_json)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_);
        rows_.push_back(trace_row{
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
    std::string input = "Say ok in one word.";
    std::string model = "gpt-5.3";
    std::size_t max_output_tokens = 1000;
    std::string reasoning_effort = "medium";
    std::string reasoning_summary = "auto";
    std::optional<std::string> trace_path;
    std::optional<std::string> playback_path;
    std::optional<std::string> playback_from;
    bool hud = false;
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
                throw std::runtime_error{"--playback-speed requires a value"};
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
                   "  --hud\n"
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

    if (!positionals.empty())
        options.input = std::move(positionals[0]);
    if (positionals.size() > 1)
        options.model = std::move(positionals[1]);
    if (positionals.size() > 2)
        throw std::runtime_error{"too many positional arguments"};

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
        .reasoning_summary =
            options.reasoning_summary == "none"
                ? std::string{}
                : options.reasoning_summary,
    };
}

std::vector<trace_row> read_trace_rows(std::string_view path)
{
    auto db = duckdb::DuckDB{nullptr};
    auto connection = duckdb::Connection{db};
    auto result = connection.Query(
        "select seq, elapsed_ms, phase, event_type, data, payload_json "
        "from read_parquet('" + sql_string(path) + "') "
        "order by seq");
    if (result->HasError())
        throw std::runtime_error{result->GetError()};

    auto rows = std::vector<trace_row>{};
    rows.reserve(static_cast<std::size_t>(result->RowCount()));
    for (duckdb::idx_t i = 0; i < result->RowCount(); ++i) {
        rows.push_back(trace_row{
            .seq = result->GetValue(0, i).GetValue<std::int64_t>(),
            .elapsed_ms = result->GetValue(1, i).GetValue<std::int64_t>(),
            .phase = result->GetValue(2, i).GetValue<std::string>(),
            .event_type = result->GetValue(3, i).GetValue<std::string>(),
            .data = result->GetValue(4, i).GetValue<std::string>(),
            .payload_json = result->GetValue(5, i).GetValue<std::string>(),
        });
    }
    return rows;
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
    const std::vector<trace_row> & rows,
    const cli_options & options)
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

    auto payload_text = row.payload_json.empty() ? row.data : row.payload_json;
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

std::string optional_text_delta(
    const stream_event & event,
    std::string_view type)
{
    if (event.type != type)
        return {};
    return event.payload.value("delta", std::string{});
}

void print_cli_event(const stream_event & event)
{
    std::cout << "event: " << event.type << '\n';

    if (auto delta =
            optional_text_delta(event, "response.output_text.delta");
        !delta.empty())
        std::cout << "text: " << delta << '\n';

    if (auto delta = optional_text_delta(
            event,
            "response.reasoning_summary_text.delta");
        !delta.empty())
        std::cout << "thinking: " << delta << '\n';

    std::cout << "data: " << event.payload.dump() << "\n\n";
}

void playback_delay(
    std::int64_t & previous_elapsed_ms,
    const trace_row & row,
    double playback_speed)
{
    if (playback_speed <= 0.0) {
        previous_elapsed_ms = row.elapsed_ms;
        return;
    }

    auto delta_ms = std::max<std::int64_t>(
        0,
        row.elapsed_ms - previous_elapsed_ms);
    previous_elapsed_ms = row.elapsed_ms;
    auto scaled_ms = static_cast<std::int64_t>(
        static_cast<double>(delta_ms) / playback_speed);
    if (scaled_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds{scaled_ms});
}

void run_cli_playback(
    const std::vector<trace_row> & rows,
    double playback_speed,
    std::size_t start_index)
{
    if (rows.empty())
        return;

    start_index = std::min(start_index, rows.size() - 1);
    auto previous_elapsed_ms = rows[start_index].elapsed_ms;
    for (auto i = start_index; i < rows.size(); ++i) {
        const auto & row = rows[i];
        playback_delay(previous_elapsed_ms, row, playback_speed);
        if (auto event = event_from_trace_row(row)) {
            print_cli_event(*event);
        } else if (row.phase == "error") {
            std::cerr << "playback error marker: " << row.data << '\n';
        }
    }
}

nxt::task<> run_live_cli(
    std::unique_ptr<nxt::io_scheduler> & scheduler,
    const llm_request & request,
    parquet_trace & trace)
{
    try {
        trace.record_request(request);

        auto transport = co_await nxt::io::net::connect_tls(
            scheduler,
            nxt::io::net::endpoint{
                .host = "api.openai.com",
                .port = 443,
            });

        auto on_event = [&](stream_event event) -> nxt::task<> {
            trace.record_event(event);
            print_cli_event(event);
            co_return;
        };

        co_await nxt::io::llm::stream_openai_responses_over(
            transport,
            request,
            on_event);
        trace.record_marker("complete");
        co_await transport.shutdown();
    } catch (const std::exception & e) {
        trace.record_marker("error", e.what());
        throw;
    }
}

struct llm_hud_state
{
    llm_request request;
    std::string status = "ready";
    std::string last_event;
    std::string error;
    std::size_t event_count = 0;
    std::size_t output_bytes = 0;
    std::size_t output_chunks = 0;
    std::size_t reasoning_bytes = 0;
    std::size_t reasoning_chunks = 0;
    bool transcript_started = false;
    bool saw_output = false;
    bool saw_reasoning = false;
    bool output_ended_with_newline = true;
    bool reasoning_ended_with_newline = true;
    bool done = false;
};

std::string fit_label(std::string text, std::size_t width)
{
    if (text.size() <= width)
        return text;
    if (width <= 3)
        return text.substr(0, width);
    return text.substr(0, width - 3) + "...";
}

std::string spinner_for(const llm_hud_state & state)
{
    if (state.done)
        return state.error.empty() ? "ok" : "!!";

    constexpr auto frames = std::array{"-", "\\", "|", "/"};
    return frames[state.event_count % frames.size()];
}

void update_hud(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    auto fn)
{
    fn(state);
    runtime.signal_damage();
}

auto build_hud(const llm_hud_state & state)
{
    using namespace nxt::tui;

    auto accent = fg(nxt::Rgba8{90, 190, 210}) | bold;
    auto muted = fg(nxt::Rgba8{150, 156, 162});
    auto normal = fg(nxt::Rgba8{220, 224, 228});
    auto good = fg(nxt::Rgba8{125, 200, 145}) | bold;
    auto bad = fg(nxt::Rgba8{235, 120, 120}) | bold;
    auto status_style = state.error.empty()
        ? (state.done ? good : normal)
        : bad;

    auto status = spinner_for(state) + " " + state.status;
    auto events = "events " + std::to_string(state.event_count);
    auto chunks = "chunks " + std::to_string(state.output_chunks);
    auto bytes = "bytes " + std::to_string(state.output_bytes);
    auto thoughts = "thinking "
        + std::to_string(state.reasoning_chunks) + "/"
        + std::to_string(state.reasoning_bytes);
    auto last = state.last_event.empty() ? "none" : state.last_event;
    auto detail = state.error.empty()
        ? std::string{"assistant text and reasoning summaries stream above"}
        : "error " + state.error;

    return column(
        row(
            text("nxtllm ", accent),
            text(status, status_style),
            text("  " + events, muted),
            text("  " + chunks, muted),
            text("  " + bytes, muted),
            text("  " + thoughts, muted)),
        hrule(),
        row(
            text("model ", muted),
            text(fit_label(state.request.model, 24), normal),
            text("  last ", muted),
            text(fit_label(last, 44), muted)),
        row(
            text("input ", muted),
            text(fit_label(state.request.input, 72), normal)),
        row(text(fit_label(detail, 96), state.error.empty() ? muted : bad)));
}

void start_hud_transcript(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state)
{
    if (state.transcript_started)
        return;

    runtime.println("user: " + state.request.input);
    runtime.println("thinking:");
    state.transcript_started = true;
}

void apply_hud_event(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    const stream_event & event)
{
    auto delta =
        optional_text_delta(event, "response.output_text.delta");
    auto reasoning_delta = optional_text_delta(
        event,
        "response.reasoning_summary_text.delta");

    if (!reasoning_delta.empty())
        runtime.print(reasoning_delta);

    if (!delta.empty()) {
        if (!state.saw_output && state.saw_reasoning
            && !state.reasoning_ended_with_newline)
            runtime.print("\n");
        if (!state.saw_output)
            runtime.print("assistant:\n");
        runtime.print(delta);
    }

    update_hud(runtime, state, [&](llm_hud_state & hud) {
        hud.last_event = event.type;
        ++hud.event_count;
        if (!reasoning_delta.empty()) {
            hud.saw_reasoning = true;
            hud.reasoning_bytes += reasoning_delta.size();
            ++hud.reasoning_chunks;
            hud.reasoning_ended_with_newline = reasoning_delta.back() == '\n';
            hud.status = "thinking";
        }
        if (!delta.empty()) {
            hud.saw_output = true;
            hud.output_bytes += delta.size();
            ++hud.output_chunks;
            hud.output_ended_with_newline = delta.back() == '\n';
            hud.status = "streaming";
        }
        if (event.type == "response.completed") {
            hud.status = "completed";
            hud.done = true;
        } else if (
            event.type == "response.failed"
            || event.type == "response.incomplete") {
            hud.status = event.type;
            hud.done = true;
        }
    });
}

void finish_hud_output(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state)
{
    if (state.saw_output && !state.output_ended_with_newline)
        runtime.print("\n");
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

    auto delta_ms = std::max<std::int64_t>(
        0,
        row.elapsed_ms - previous_elapsed_ms);
    previous_elapsed_ms = row.elapsed_ms;
    auto scaled_ms = static_cast<std::int64_t>(
        static_cast<double>(delta_ms) / playback_speed);
    if (scaled_ms > 0)
        co_await runtime.sleep(std::chrono::milliseconds{scaled_ms});
}

nxt::task<> run_hud_playback(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    const std::vector<trace_row> & rows,
    double playback_speed,
    std::size_t start_index)
{
    using namespace std::chrono_literals;

    start_hud_transcript(runtime, state);
    update_hud(runtime, state, [](llm_hud_state & hud) {
        hud.status = "replaying";
    });

    if (rows.empty()) {
        update_hud(runtime, state, [](llm_hud_state & hud) {
            hud.status = "completed";
            hud.done = true;
        });
        co_await runtime.sleep(1500ms);
        runtime.request_shutdown();
        co_return;
    }

    start_index = std::min(start_index, rows.size() - 1);
    auto previous_elapsed_ms = rows[start_index].elapsed_ms;
    for (auto i = start_index; i < rows.size(); ++i) {
        const auto & row = rows[i];
        if (runtime.shutdown_requested())
            co_return;

        co_await playback_delay(
            runtime,
            previous_elapsed_ms,
            row,
            playback_speed);

        if (auto event = event_from_trace_row(row)) {
            apply_hud_event(runtime, state, *event);
        } else if (row.phase == "error") {
            runtime.println("playback error marker: " + row.data);
            update_hud(runtime, state, [&](llm_hud_state & hud) {
                hud.status = "error";
                hud.error = row.data;
                hud.done = true;
            });
        } else if (row.phase == "complete") {
            update_hud(runtime, state, [](llm_hud_state & hud) {
                hud.status = "completed";
                hud.done = true;
            });
        }
    }

    finish_hud_output(runtime, state);
    update_hud(runtime, state, [](llm_hud_state & hud) {
        if (!hud.done) {
            hud.status = "completed";
            hud.done = true;
        }
    });

    co_await runtime.sleep(1500ms);
    runtime.request_shutdown();
}

nxt::task<> run_hud_live(
    nxt::ui::UIRuntime & runtime,
    llm_hud_state & state,
    parquet_trace & trace)
{
    using namespace std::chrono_literals;

    auto exit_delay = 1500ms;
    start_hud_transcript(runtime, state);

    try {
        trace.record_request(state.request);
        update_hud(runtime, state, [](llm_hud_state & hud) {
            hud.status = "connecting";
        });

        auto transport = co_await nxt::io::net::connect_tls(
            runtime.scheduler_handle(),
            nxt::io::net::endpoint{
                .host = "api.openai.com",
                .port = 443,
            });

        update_hud(runtime, state, [](llm_hud_state & hud) {
            hud.status = "streaming";
        });

        auto on_event = [&](stream_event event) -> nxt::task<> {
            trace.record_event(event);
            apply_hud_event(runtime, state, event);
            co_return;
        };

        co_await nxt::io::llm::stream_openai_responses_over(
            transport,
            state.request,
            on_event,
            runtime.get_stop_token());
        trace.record_marker("complete");
        co_await transport.shutdown();

        finish_hud_output(runtime, state);
        update_hud(runtime, state, [](llm_hud_state & hud) {
            if (!hud.done) {
                hud.status = "completed";
                hud.done = true;
            }
        });
    } catch (const std::exception & e) {
        if (runtime.shutdown_requested()) {
            trace.record_marker("cancelled");
            finish_hud_output(runtime, state);
            update_hud(runtime, state, [](llm_hud_state & hud) {
                hud.status = "cancelled";
                hud.done = true;
            });
            co_return;
        }

        trace.record_marker("error", e.what());
        runtime.println(std::string{"nxtllm error: "} + e.what());
        update_hud(runtime, state, [&](llm_hud_state & hud) {
            hud.status = "error";
            hud.error = e.what();
            hud.done = true;
        });
        exit_delay = 3000ms;
    }

    co_await runtime.sleep(exit_delay);
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
            auto rows = read_trace_rows(*options.playback_path);
            auto start_index =
                playback_start_index(rows, options.playback_from);
            auto state = llm_hud_state{};
            state.request = request_from_trace(rows, options);
            if (options.hud) {
                return nxt::ui::run(
                    std::move(state),
                    build_hud,
                    [rows = std::move(rows),
                     speed = options.playback_speed,
                     start_index](
                        nxt::ui::UIRuntime & runtime,
                        llm_hud_state & hud) -> nxt::task<> {
                        co_await run_hud_playback(
                            runtime,
                            hud,
                            rows,
                            speed,
                            start_index);
                    });
            }

            run_cli_playback(rows, options.playback_speed, start_index);
            return 0;
        }

        const char * api_key = std::getenv("OPENAI_API_KEY");
        if (api_key == nullptr || std::string_view{api_key}.empty())
            throw std::runtime_error{"OPENAI_API_KEY is not set"};

        trace = parquet_trace{options.trace_path};
        auto request = make_request(options, api_key);

        if (options.hud) {
            auto state = llm_hud_state{};
            state.request = std::move(request);
            auto status = nxt::ui::run(
                std::move(state),
                build_hud,
                [&trace](
                    nxt::ui::UIRuntime & runtime,
                    llm_hud_state & hud) -> nxt::task<> {
                    co_await run_hud_live(runtime, hud, trace);
                });
            trace.write();
            if (trace.output_path())
                std::cout << "wrote trace parquet: " << *trace.output_path()
                          << '\n';
            return status;
        }

        auto scheduler =
            nxt::io_scheduler::make_unique(nxt::io_scheduler::options{});
        nxt::sync_wait(run_live_cli(scheduler, request, trace));
        trace.write();
        if (trace.output_path())
            std::cout << "wrote trace parquet: " << *trace.output_path()
                      << '\n';
        return 0;
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
