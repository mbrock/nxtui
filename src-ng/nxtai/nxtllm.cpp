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

#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

auto header_layout(const live_state & state)
{
    namespace tt = nxt::ai::tool_tui;
    auto summary = std::format("{}  {}", state.model, state.status);
    return nxt::tui::row(
        std::vector<nxt::tui::AnyLayout>{
            tt::chip(" nxtllm ", tt::slate_950, tt::amber_300,
                     nxt::Emphasis::bold),
            nxt::tui::flex_text(
                std::move(summary),
                nxt::tui::fg(tt::slate_400) | nxt::tui::bg(tt::band_bg)),
        });
}

auto live_layout(const live_state & state)
{
    namespace tt = nxt::ai::tool_tui;
    auto rows = std::vector<nxt::tui::AnyLayout>{};
    rows.push_back(header_layout(state));

    if (!state.assistant_text.empty()) {
        rows.push_back(nxt::tui::text_lines(
            join_lines(last_lines(state.assistant_text, 8)),
            nxt::tui::fg(tt::slate_300)));
    } else {
        rows.push_back(nxt::tui::text(
            "waiting for first tokens", nxt::tui::fg(tt::slate_700)));
    }

    if (!state.turn.calls.empty())
        rows.push_back(tt::render_turn(state.turn));

    return nxt::tui::surface(
        nxt::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxt::DEFAULT_EMPHASIS,
        },
        nxt::tui::column(std::move(rows)));
}

nxt::rt::task<void> render_live_until_done(
    nxt::rt::ui_scope ui,
    const live_state & state)
{
    while (!state.done) {
        ui.draw(live_layout(state));
        co_await nxt::rt::op::timeout::after(frame_interval);
    }

    ui.draw(live_layout(state));
    ui.request_shutdown();
}

nxt::rt::task<response_stream_result> stream_openai_response(
    const llm_request & request,
    live_state * live = nullptr)
{
    if (live != nullptr)
        live->status = "connecting";

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
    if (live != nullptr)
        live->status = "tls handshake";
    co_await tls.handshake("api.openai.com");

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
        } else if (event.type == "response.output_text.delta") {
            auto delta = event.read<nxt::ai::openai::text_delta_event>();
            if (live != nullptr) {
                live->status = "streaming";
                live->assistant_text += delta.delta;
            } else {
                std::cout << delta.delta << std::flush;
            }
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

    if (live == nullptr)
        std::cout << '\n';
    co_return result;
}

template<typename ToolSet>
nxt::rt::task<void> run_agent_loop(
    llm_request request,
    ToolSet tools,
    live_state * live = nullptr,
    std::size_t max_steps = 32)
{
    auto original = request;
    auto stateless_input = nxt::ai::responses::input_items_from_request(request);
    prepare_tool_request(request, tools);

    for (std::size_t step = 0; step < max_steps; ++step) {
        if (live != nullptr)
            live->status = std::format("turn {}", step + 1);
        auto response = co_await stream_openai_response(request, live);
        auto calls =
            nxt::ai::tools::function_calls_from_items(response.output_items);
        if (calls.empty())
            co_return;

        if (request.store && !response.response_id)
            throw nxt::rt::runtime_error{
                "tool call response had no response id"};

        auto started = std::chrono::steady_clock::now();
        if (live != nullptr) {
            live->status =
                std::format("running {} tool call(s)", calls.size());
            live->turn.calls.clear();
            live->turn.calls.reserve(calls.size());
            for (const auto & call : calls) {
                live->turn.calls.push_back(
                    nxt::ai::tool_tui::call_view{
                        .name = call.name,
                        .arguments = call.arguments,
                        .output = {},
                        .state = nxt::ai::tool_tui::status::running,
                        .elapsed_ms = -1,
                    });
            }
        } else {
            std::cout << "[tools] running " << calls.size()
                      << " tool call(s)\n";
        }

        auto results =
            co_await nxt::ai::tools::run_function_tool_batch(tools, calls);
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto & result = results[i];
            if (live != nullptr) {
                if (i < live->turn.calls.size()) {
                    auto & call = live->turn.calls[i];
                    call.state = result.result.failed
                        ? nxt::ai::tool_tui::status::error
                        : nxt::ai::tool_tui::status::ok;
                    call.output = result.result.output;
                    call.elapsed_ms = static_cast<int>(elapsed);
                }
            } else {
                std::cout << "[tool] " << result.call.name << " -> "
                          << (result.result.failed ? "failed" : "ok") << " ("
                          << result.result.output.size() << " bytes)\n";
            }
        }

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

    auto live = live_state{
        .model = request.model,
        .status = "starting",
        .assistant_text = {},
        .turn = {},
        .done = false,
    };
    auto publisher = nxt::rt::catching_deed<void>{};

    co_await nxt::rt::with_ui_zone([&](nxt::rt::ui_scope ui)
        -> nxt::rt::task<void> {
        publisher = ui.fork(render_live_until_done(ui, live)).cope();
        try {
            co_await run_agent_loop(std::move(request), std::move(tools), &live);
            live.status = "done";
            live.done = true;
            ui.request_shutdown();
        } catch (...) {
            live.status = "failed";
            live.done = true;
            ui.request_shutdown();
            throw;
        }
    }, {}, frame_interval);

    auto published = std::move(publisher).get();
    if (!published)
        nxt::rt::rethrow(published.error());

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
