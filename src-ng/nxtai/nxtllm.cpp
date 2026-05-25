#include <nxt/rt.hpp>
#include <nxt/rt/net.hpp>
#include <nxt/rt/tls.hpp>
#include <nxt/rt/ui_runtime.hpp>
#include <nxt/http.hpp>
#include <nxtai/ng_agent_tools.hpp>
#include <nxtai/openai_types.hpp>
#include <nxtai/responses_request.hpp>
#include <nxtai/tool_batch.hpp>
#include <nxtai/tool_tui.hpp>
#include <nxtai/trace_tui.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using llm_request = nxt::ai::responses::openai_responses_request;

constexpr std::size_t default_max_output_tokens = 128000;
constexpr auto frame_interval = std::chrono::milliseconds{16};

struct cli_options
{
    std::string model = "gpt-5.4-mini";
    std::size_t max_output_tokens = default_max_output_tokens;
    std::string reasoning_effort = "medium";
    std::string reasoning_summary = "auto";
    bool store = true;
    bool dump_request = false;
    std::optional<std::string> oneshot_prompt;
};

[[noreturn]] void print_help_and_exit()
{
    std::cout
        << "usage: nxtllm [options] [prompt...]\n"
           "  ng migration build: streams one-shot Responses text on nxt::rt\n"
           "\n"
           "  -m, --model MODEL                 (default: gpt-5.4-mini)\n"
           "  --max-output-tokens N\n"
           "  --reasoning-effort none|low|medium|high|xhigh\n"
           "  --reasoning-summary auto|concise|detailed|none\n"
           "  --stateless                       resend local transcript instead of using stored response ids\n"
           "  --dump-request                    print serialized Responses JSON\n";
    std::exit(EXIT_SUCCESS);
}

cli_options parse_args(int argc, char ** argv)
{
    auto options = cli_options{};
    auto positionals = std::vector<std::string>{};

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--model" || arg == "-m") {
            if (i + 1 >= argc)
                throw std::runtime_error{"--model requires a value"};
            options.model = argv[++i];
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
        if (arg == "--stateless") {
            options.store = false;
            continue;
        }
        if (arg == "--dump-request") {
            options.dump_request = true;
            continue;
        }
        if (arg == "--help" || arg == "-h")
            print_help_and_exit();

        positionals.emplace_back(arg);
    }

    if (!positionals.empty()) {
        auto joined = std::string{};
        for (std::size_t i = 0; i < positionals.size(); ++i) {
            if (i > 0)
                joined += ' ';
            joined += positionals[i];
        }
        options.oneshot_prompt = std::move(joined);
    }

    return options;
}

std::string env_string(const char * name)
{
    auto * value = std::getenv(name);
    if (value == nullptr)
        return {};
    return value;
}

llm_request make_request(const cli_options & options)
{
    return llm_request{
        .api_key = env_string("OPENAI_API_KEY"),
        .model = options.model,
        .input = options.oneshot_prompt.value_or(""),
        .previous_response_id = {},
        .max_output_tokens = options.max_output_tokens,
        .reasoning_effort = options.reasoning_effort,
        .reasoning_summary = options.reasoning_summary == "none"
                                 ? std::string{}
                                 : options.reasoning_summary,
        .store = options.store,
    };
}

struct stream_event
{
    std::string type;
    std::string data;

    template<typename T, auto Opts = nxt::ai::openai::json_read_opts>
    [[nodiscard]] T read() const
    {
        auto payload = T{};
        glz::ex::read<Opts>(payload, data);
        return payload;
    }
};

struct response_stream_result
{
    std::vector<nxt::ai::openai::raw_json> output_items;
    std::optional<std::string> response_id;
    bool completed = false;
};

struct live_state
{
    std::string model;
    std::string status = "starting";
    std::string assistant_text;
    nxt::ai::tool_tui::turn_view turn;
    std::chrono::steady_clock::time_point last_publish{};
    bool done = false;
};

[[nodiscard]] bool response_status_is_success(
    const nxt::rt::http::response_head & head)
{
    return head.status >= 200 && head.status < 300;
}

[[nodiscard]] bool response_content_type_is(
    const nxt::rt::http::response_head & head,
    std::string_view expected)
{
    auto value = nxt::rt::http::header_value(head, "content-type");
    if (!value)
        return false;
    auto semicolon = value->find(';');
    auto media_type = nxt::rt::http::trim_ascii(value->substr(0, semicolon));
    return nxt::rt::http::iequals(media_type, expected);
}

template<typename ToolSet>
void prepare_tool_request(llm_request & request, const ToolSet & tools)
{
    if (nxt::ai::tools::empty(tools))
        return;
    request.tools = nxt::ai::tools::function_tool_definitions(tools);
    if (!request.store)
        request.include = {"reasoning.encrypted_content"};
}

void append_stateless_turn(
    std::vector<nxt::ai::openai::raw_json> & input,
    std::vector<nxt::ai::openai::raw_json> output_items,
    std::vector<nxt::ai::openai::raw_json> tool_outputs)
{
    for (auto & item : output_items)
        input.push_back(std::move(item));
    for (auto & output : tool_outputs)
        input.push_back(std::move(output));
}

std::vector<std::string> last_lines(std::string_view text, std::size_t max)
{
    auto lines = std::vector<std::string>{};
    auto current = std::string{};
    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(std::move(current));
            current = {};
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty() || lines.empty())
        lines.push_back(std::move(current));

    if (lines.size() > max)
        lines.erase(lines.begin(), lines.begin() + (lines.size() - max));
    return lines;
}

std::string join_lines(const std::vector<std::string> & lines)
{
    auto out = std::string{};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0)
            out += '\n';
        out += lines[i];
    }
    return out;
}

void append_thought_delta(live_state & state, std::string_view delta)
{
    static constexpr auto max_thought_bytes = std::size_t{4096};
    state.turn.thought.append(delta);
    if (state.turn.thought.size() <= max_thought_bytes)
        return;
    auto drop = state.turn.thought.size() - max_thought_bytes;
    auto newline = state.turn.thought.find('\n', drop);
    if (newline != std::string::npos)
        drop = newline + 1;
    state.turn.thought.erase(0, drop);
}

auto header_layout(const live_state & state)
{
    namespace tt = nxt::ai::tool_tui;
    auto summary = std::format("{}  {}", state.model, state.status);
    return nxt::tui::row(
        tt::chip(" nxtllm ", tt::slate_950, tt::amber_300,
                 nxt::Emphasis::bold),
        nxt::tui::flex_text(
            std::move(summary),
            nxt::tui::fg(tt::slate_400) | nxt::tui::bg(tt::band_bg)));
}

auto assistant_preview_layout(const live_state & state)
{
    namespace tt = nxt::ai::tool_tui;
    auto preview = join_lines(last_lines(state.assistant_text, 8));
    return nxt::tui::either(
        !state.assistant_text.empty(),
        nxt::tui::empty(),
        nxt::tui::text_lines(
            std::move(preview), nxt::tui::fg(tt::slate_300)));
}

auto activity_layout(const live_state & state)
{
    return nxt::tui::either(
        !state.turn.thought.empty() || !state.turn.calls.empty(),
        nxt::tui::empty(),
        nxt::ai::tool_tui::render_turn(state.turn));
}

auto live_layout(const live_state & state)
{
    namespace tt = nxt::ai::tool_tui;
    return nxt::tui::surface(
        nxt::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxt::DEFAULT_EMPHASIS,
        },
        nxt::tui::column(
            header_layout(state),
            assistant_preview_layout(state),
            activity_layout(state)));
}

class plain_agent_presenter
{
public:
    void status(std::string, bool = true)
    {
    }

    void publish(bool = false)
    {
    }

    void reasoning_started()
    {
    }

    void reasoning_delta(std::string_view)
    {
    }

    nxt::rt::task<void> reasoning_done(std::string)
    {
        co_return;
    }

    void output_delta(std::string_view delta)
    {
        std::cout << delta << std::flush;
    }

    void finish_plain_output()
    {
        std::cout << '\n';
    }

    nxt::rt::task<void> finish_assistant()
    {
        co_return;
    }

    void begin_tool_batch(
        const std::vector<nxt::ai::tools::function_call> & calls)
    {
        std::cout << "[tools] running " << calls.size() << " tool call(s)\n";
    }

    nxt::rt::task<void> finish_tool_batch(
        const std::vector<nxt::ai::tools::function_call_result> & results,
        int)
    {
        for (const auto & result : results) {
            std::cout << "[tool] " << result.call.name << " -> "
                      << (result.result.failed ? "failed" : "ok") << " ("
                      << result.result.output.size() << " bytes)\n";
        }
        co_return;
    }

    nxt::rt::task<void> tls_ready(
        const std::shared_ptr<nxt::rt::trace_context> &,
        const nxt::rt::trace_span &)
    {
        co_return;
    }
};

class hud_agent_presenter
{
public:
    hud_agent_presenter(nxt::rt::ui_scope & ui, live_state & state)
        : hud_ui_{ui}
        , hud_{state}
    {
    }

    void status(std::string text, bool force = true)
    {
        hud_.status = std::move(text);
        publish(force);
    }

    void publish(bool force = false)
    {
        auto now = std::chrono::steady_clock::now();
        if (!force
            && hud_.last_publish != std::chrono::steady_clock::time_point{}
            && now - hud_.last_publish < frame_interval)
            return;
        hud_.last_publish = now;
        hud_ui_.draw(live_layout(hud_));
    }

    void reasoning_started()
    {
        hud_.status = "thinking";
        publish(true);
    }

    void reasoning_delta(std::string_view delta)
    {
        hud_.status = "thinking";
        append_thought_delta(hud_, delta);
        publish();
    }

    nxt::rt::task<void> reasoning_done(std::string text)
    {
        if (!text.empty())
            hud_.turn.thought = std::move(text);
        auto summary = std::move(hud_.turn.thought);
        hud_.turn.thought.clear();
        hud_.status = "thinking done";
        if (!summary.empty())
            co_await hud_ui_.print(
                nxt::ai::tool_tui::thought_block(std::move(summary)));
        publish(true);
    }

    void output_delta(std::string_view delta)
    {
        auto first_stream_delta = hud_.status != "streaming";
        hud_.status = "streaming";
        hud_.assistant_text += delta;
        publish(first_stream_delta);
    }

    void finish_plain_output()
    {
    }

    nxt::rt::task<void> finish_assistant()
    {
        if (hud_.assistant_text.empty())
            co_return;
        co_await hud_ui_.print(
            nxt::ai::tool_tui::assistant_block(
                std::move(hud_.assistant_text)));
        hud_.assistant_text.clear();
        publish(true);
    }

    void begin_tool_batch(
        const std::vector<nxt::ai::tools::function_call> & calls)
    {
        hud_.status = std::format("running {} tool call(s)", calls.size());
        hud_.turn.calls.clear();
        hud_.turn.calls.reserve(calls.size());
        for (const auto & call : calls) {
            hud_.turn.calls.push_back(
                nxt::ai::tool_tui::call_view{
                    .name = call.name,
                    .arguments = call.arguments,
                    .output = {},
                    .state = nxt::ai::tool_tui::status::running,
                    .elapsed_ms = -1,
                });
        }
        publish(true);
    }

    nxt::rt::task<void> finish_tool_batch(
        const std::vector<nxt::ai::tools::function_call_result> & results,
        int elapsed_ms)
    {
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto & result = results[i];
            if (i < hud_.turn.calls.size()) {
                auto & call = hud_.turn.calls[i];
                call.state = result.result.failed
                    ? nxt::ai::tool_tui::status::error
                    : nxt::ai::tool_tui::status::ok;
                call.output = result.result.output;
                call.elapsed_ms = elapsed_ms;
            }
            publish(true);
        }

        if (!hud_.turn.calls.empty()) {
            co_await hud_ui_.print(nxt::ai::tool_tui::render_turn(hud_.turn));
            hud_.turn.calls.clear();
            publish(true);
        }
    }

    nxt::rt::task<void> tls_ready(
        const std::shared_ptr<nxt::rt::trace_context> & trace,
        const nxt::rt::trace_span & tls_span)
    {
        if (trace != nullptr && tls_span)
            co_await hud_ui_.print(
                nxt::ai::trace_tui::render_span_waterfall(
                    *trace,
                    tls_span,
                    {
                        .label = "",
                        .detail = "TLS 1.3",
                        .subject = "api.openai.com",
                        .accent = nxt::ai::tool_tui::teal_300,
                    }));
        else
            co_await hud_ui_.print_block(
                "tls  api.openai.com  TLS 1.3 handshake\n");
        publish(true);
    }

    void done()
    {
        status("done");
        hud_.done = true;
    }

    void failed()
    {
        status("failed");
        hud_.done = true;
    }

private:
    nxt::rt::ui_scope & hud_ui_;
    live_state & hud_;
};

template<typename Presenter>
nxt::rt::task<response_stream_result> stream_openai_response(
    const llm_request & request,
    Presenter & presenter)
{
    presenter.status("connecting");

    auto socket = co_await nxt::rt::net::connect_tcp("api.openai.com", "443");
    auto socket_output = nxt::rt::byte_writer<nxt::rt::socket_sink>{
        nxt::rt::socket_sink{socket.get()},
        4096,
    };
    auto source = nxt::rt::socket_source{socket.get()};
    auto input_storage = std::vector<std::byte>(18 * 1024);
    auto reader = nxt::rt::byte_reader<nxt::rt::socket_source>{
        source,
        std::span{input_storage},
    };

    auto tls = nxt::rt::tls::tls13_client_session{reader, socket_output};
    presenter.status("tls handshake");
    auto trace = nxt::rt::current_trace_context();
    auto tls_span = nxt::rt::trace_span{};
    if (trace != nullptr) {
        tls_span = trace->start_span(
            "tls.handshake",
            nxt::rt::current_trace_span_id(),
            {
                {"net.peer.name", "api.openai.com"},
                {"tls.version", "1.3"},
            });
    }
    auto active_tls_spans = std::vector<nxt::rt::trace_span>{};
    try {
        co_await tls.handshake(
            "api.openai.com",
            [&](const nxt::rt::tls::handshake_progress & progress)
                -> nxt::rt::task<> {
                if (progress.kind
                    == nxt::rt::tls::handshake_progress_kind::begin) {
                    presenter.status(
                        std::format(
                            "tls: {}",
                            nxt::ai::trace_tui::display_name(
                                progress.name)));
                    if (trace != nullptr && tls_span)
                        active_tls_spans.push_back(
                            trace->start_span(
                                std::string{progress.name},
                                tls_span.span_id()));
                    co_return;
                }

                if (progress.kind
                    == nxt::rt::tls::handshake_progress_kind::end) {
                    for (auto it = active_tls_spans.rbegin();
                         it != active_tls_spans.rend();
                         ++it) {
                        if (it->name() == progress.name) {
                            it->finish("ok");
                            active_tls_spans.erase(std::next(it).base());
                            break;
                        }
                    }
                    co_return;
                }

                if (trace != nullptr && tls_span)
                    tls_span.event(std::string{progress.name});
                presenter.status(
                    std::format(
                        "tls: {}",
                        nxt::ai::trace_tui::display_name(progress.name)));
            });
        if (tls_span)
            tls_span.finish("ok");
    } catch (...) {
        for (auto & span : active_tls_spans)
            span.finish("error");
        if (tls_span)
            tls_span.finish("error");
        throw;
    }
    co_await presenter.tls_ready(trace, tls_span);

    auto http_request =
        nxt::ai::responses::openai_responses_http_request(request);
    for (auto & header : http_request.headers) {
        if (header.name == "Connection")
            header.value = "close";
    }
    auto request_text = nxt::http::serialize(http_request);
    co_await tls.write_all(request_text);

    auto http_storage = std::vector<std::byte>(18 * 1024);
    auto http_reader =
        nxt::rt::byte_reader{tls, std::span{http_storage}};
    auto head = co_await nxt::rt::http::read_response_head(http_reader);
    if (!response_status_is_success(head))
        throw nxt::rt::runtime_error{
            "OpenAI Responses HTTP error: " + std::to_string(head.status)
            + " " + head.reason};
    if (!response_content_type_is(head, "text/event-stream"))
        throw nxt::rt::runtime_error{
            "OpenAI Responses expected text/event-stream"};

    auto body = nxt::rt::http::read_response_body(http_reader, head);
    auto sse_storage = std::vector<std::byte>(18 * 1024);
    auto sse_reader = nxt::rt::byte_reader{body, std::span{sse_storage}};
    auto result = response_stream_result{};
    while (auto sse = co_await nxt::rt::http::parse_sse_event(sse_reader)) {
        if (sse->data == "[DONE]")
            break;

        auto terminal = sse->type == "response.completed"
                        || sse->type == "response.incomplete"
                        || sse->type == "response.failed";
        auto event = stream_event{
            .type = std::move(sse->type),
            .data = std::move(sse->data),
        };

        if (event.type == "response.created") {
            auto payload = event.read<nxt::ai::openai::response_event>();
            if (!payload.response.id.empty())
                result.response_id = std::move(payload.response.id);
        } else if (event.type == "response.output_item.done") {
            auto payload = event.read<nxt::ai::openai::output_item_event>();
            result.output_items.push_back(std::move(payload.item));
        } else if (event.type == "response.reasoning_summary_part.added") {
            presenter.reasoning_started();
        } else if (event.type == "response.reasoning_summary_text.delta") {
            auto delta =
                event.read<
                    nxt::ai::openai::reasoning_summary_text_delta_event>();
            presenter.reasoning_delta(delta.delta);
        } else if (event.type == "response.reasoning_summary_text.done") {
            auto done =
                event.read<
                    nxt::ai::openai::reasoning_summary_text_done_event>();
            co_await presenter.reasoning_done(std::move(done.text));
        } else if (event.type == "response.output_text.delta") {
            auto delta = event.read<nxt::ai::openai::text_delta_event>();
            presenter.output_delta(delta.delta);
        } else if (
            event.type == "response.failed"
            || event.type == "response.incomplete") {
            throw nxt::rt::runtime_error{
                "OpenAI Responses terminal event: " + event.type};
        }

        if (event.type == "response.completed")
            result.completed = true;
        if (terminal)
            break;
    }

    presenter.finish_plain_output();
    co_return result;
}

template<typename ToolSet, typename Presenter>
nxt::rt::task<void> run_agent_loop(
    llm_request request,
    ToolSet tools,
    Presenter & presenter,
    std::size_t max_steps = 32)
{
    auto original = request;
    auto stateless_input = nxt::ai::responses::input_items_from_request(request);
    prepare_tool_request(request, tools);

    for (std::size_t step = 0; step < max_steps; ++step) {
        presenter.status(std::format("turn {}", step + 1));
        auto response = co_await stream_openai_response(request, presenter);
        auto calls =
            nxt::ai::tools::function_calls_from_items(response.output_items);
        if (calls.empty()) {
            co_await presenter.finish_assistant();
            co_return;
        }

        if (request.store && !response.response_id)
            throw nxt::rt::runtime_error{
                "tool call response had no response id"};

        auto started = std::chrono::steady_clock::now();
        presenter.begin_tool_batch(calls);

        auto results =
            co_await nxt::ai::tools::run_function_tool_batch(tools, calls);
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        co_await presenter.finish_tool_batch(
            results, static_cast<int>(elapsed));

        auto outputs = nxt::ai::tools::output_items_from_results(results);
        request = original;
        request.input.clear();
        request.input_items.clear();
        request.previous_response_id.clear();

        if (request.store) {
            request.input_items = std::move(outputs);
            request.previous_response_id = *response.response_id;
        } else {
            append_stateless_turn(
                stateless_input,
                std::move(response.output_items),
                std::move(outputs));
            request.input_items = stateless_input;
        }
        prepare_tool_request(request, tools);
    }

    throw nxt::rt::runtime_error{"too many tool call turns"};
}

nxt::rt::task<int> run_nxtllm(cli_options options)
{
    auto request = make_request(options);
    auto tools = nxt::ai::agent_tools::for_agent();

    if (!options.oneshot_prompt) {
        std::cout
            << "nxtllm is now on nxt::rt; the interactive HUD is still being "
               "ported.\n"
            << "Pass a prompt for one-shot request construction, or use "
               "--dump-request to inspect the JSON envelope.\n";
        co_return EXIT_SUCCESS;
    }

    prepare_tool_request(request, tools);

    if (options.dump_request) {
        std::cout << nxt::ai::responses::openai_responses_body(request)
                  << '\n';
        co_return EXIT_SUCCESS;
    }

    if (request.api_key.empty()) {
        std::cerr
            << "nxtllm: OPENAI_API_KEY is not set; streaming is wired through "
               "nxt::rt, but it needs credentials.\n"
            << "Try --dump-request to inspect the ng Responses payload.\n";
        co_return EXIT_FAILURE;
    }

    if (::isatty(STDOUT_FILENO) == 0) {
        auto presenter = plain_agent_presenter{};
        co_await run_agent_loop(
            std::move(request), std::move(tools), presenter);
        co_return EXIT_SUCCESS;
    }

    auto live = live_state{
        .model = request.model,
        .status = "starting",
        .assistant_text = {},
        .turn = {},
        .last_publish = {},
        .done = false,
    };

    auto trace = std::make_shared<nxt::rt::trace_context>();
    auto root_span = trace->start_span(
        "nxtllm.request",
        {},
        {{"model", request.model}});

    try {
        co_await nxt::rt::with_env<nxt::rt::trace_context_key>(
            trace,
            [&]() -> nxt::rt::task<void> {
            co_await nxt::rt::with_env<nxt::rt::trace_current_span_key>(
                root_span.span_id(),
                [&]() -> nxt::rt::task<void> {
                co_await nxt::rt::with_ui_zone([&](nxt::rt::ui_scope ui)
                    -> nxt::rt::task<void> {
                    auto presenter = hud_agent_presenter{ui, live};
                    presenter.publish(true);
                    try {
                        co_await run_agent_loop(
                            std::move(request),
                            std::move(tools),
                            presenter);
                        presenter.done();
                        ui.request_shutdown();
                    } catch (...) {
                        presenter.failed();
                        ui.request_shutdown();
                        throw;
                    }
                }, {}, frame_interval);
            });
        });
        root_span.finish("ok");
    } catch (...) {
        root_span.finish("error");
        throw;
    }

    if (!live.assistant_text.empty())
        co_await nxt::rt::write_stdout_all(live.assistant_text + "\n");
    co_return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char ** argv)
try {
    auto rt = nxt::rt::runtime{};
    return rt.run(run_nxtllm(parse_args(argc, argv)));
} catch (std::exception const & error) {
    std::cerr << "nxtllm: " << error.what() << '\n';
    return EXIT_FAILURE;
}
