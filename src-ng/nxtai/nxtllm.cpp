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

#include <algorithm>
#include <chrono>
#include <cstdint>
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
using function_call = nxt::ai::tools::function_call;
using function_call_result = nxt::ai::tools::function_call_result;
using tool_result = nxt::ai::tools::tool_result;

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

struct network_hud_state
{
    std::string phase;
    std::size_t socket_rx = 0;
    std::size_t socket_tx = 0;
    double socket_rx_bps = 0.0;
    double socket_tx_bps = 0.0;
    std::size_t sse_events = 0;
};

struct stream_phase_result
{
    response_stream_result response;
    std::string assistant_text;
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

std::string format_compact_bytes(std::size_t bytes)
{
    if (bytes < 1000)
        return std::format("{}", bytes);
    auto kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 10.0)
        return std::format("{:.1f}K", kb);
    if (kb < 1000.0)
        return std::format("{:.0f}K", kb);
    auto mb = kb / 1024.0;
    if (mb < 10.0)
        return std::format("{:.1f}M", mb);
    return std::format("{:.0f}M", mb);
}

std::string format_compact_rate(double bytes_per_second)
{
    return format_compact_bytes(
        static_cast<std::size_t>(std::max(bytes_per_second, 0.0))) + "/s";
}

nxt::Rgba8 blend(nxt::Rgba8 a, nxt::Rgba8 b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    auto channel = [=](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(
            static_cast<double>(x)
            + (static_cast<double>(y) - static_cast<double>(x)) * t);
    };
    return nxt::Rgba8{
        channel(a.r(), b.r()),
        channel(a.g(), b.g()),
        channel(a.b(), b.b()),
    };
}

nxt::Rgba8 rate_bg(nxt::Rgba8 color, double bytes_per_second)
{
    static constexpr auto max_display_rate = 32.0 * 1024.0;
    auto fraction =
        std::clamp(bytes_per_second / max_display_rate, 0.0, 1.0);
    return blend(nxt::ai::tool_tui::slate_900, color, 0.12 + 0.58 * fraction);
}

enum class cell_align { left, right };

std::string fit_cell(std::string s, std::size_t width, cell_align align)
{
    if (width == 0)
        return {};
    auto display_width = static_cast<std::size_t>(nxt::tui::utf8_width(s).count());
    if (display_width > width) {
        if (width == 1)
            return s.substr(0, 1);
        s.resize(width - 1);
        s += "…";
        return s;
    }

    auto pad = std::string(width - display_width, ' ');
    if (align == cell_align::right)
        return pad + std::move(s);
    s += pad;
    return s;
}

auto fixed_cell(
    nxt::width_t width,
    std::string s,
    nxt::tui::Style style,
    cell_align align = cell_align::left)
{
    auto cells = static_cast<std::size_t>(width.count());
    return nxt::tui::line_text(
        nxt::tui::WidthHint::fixed(width),
        [s = std::move(s), cells, align](nxt::width_t) {
            return fit_cell(s, cells, align);
        },
        style);
}

auto rate_cell(std::string label, double bytes_per_second, nxt::Rgba8 color)
{
    namespace tt = nxt::ai::tool_tui;
    auto style = nxt::tui::fg(tt::slate_300) | nxt::tui::bg(rate_bg(color, bytes_per_second));
    return nxt::tui::line_text(
        nxt::tui::WidthHint{7 * nxt::ch, 1.0 * nxt::one},
        [label = std::move(label)](nxt::width_t width) {
            return fit_cell(label, width.count(), cell_align::right);
        },
        style);
}

void append_thought_delta(std::string & thought, std::string_view delta)
{
    static constexpr auto max_thought_bytes = std::size_t{4096};
    thought.append(delta);
    if (thought.size() <= max_thought_bytes)
        return;
    auto drop = thought.size() - max_thought_bytes;
    auto newline = thought.find('\n', drop);
    if (newline != std::string::npos)
        drop = newline + 1;
    thought.erase(0, drop);
}

auto header_layout(std::string_view model, std::string_view status)
{
    namespace tt = nxt::ai::tool_tui;
    auto summary = std::format("{}  {}", model, status);
    return nxt::tui::row(
        tt::chip(" nxtllm ", tt::slate_950, tt::amber_300,
                 nxt::Emphasis::bold),
        nxt::tui::flex_text(
            std::move(summary),
            nxt::tui::fg(tt::slate_400) | nxt::tui::bg(tt::band_bg)));
}

auto assistant_preview_layout(std::string_view assistant_text)
{
    namespace tt = nxt::ai::tool_tui;
    auto preview = join_lines(last_lines(assistant_text, 8));
    return nxt::tui::when(
        !assistant_text.empty(),
        nxt::tui::text_lines(
            std::move(preview), nxt::tui::fg(tt::slate_300)));
}

auto stream_activity_layout(
    std::string_view thought,
    std::string_view assistant_text)
{
    return nxt::tui::column(
        nxt::tui::when(
            !thought.empty(),
            nxt::ai::tool_tui::thought_block(std::string{thought})),
        nxt::tui::when(
            !assistant_text.empty(),
            assistant_preview_layout(assistant_text)));
}

auto network_footer_layout(const network_hud_state & net)
{
    namespace tt = nxt::ai::tool_tui;
    auto phase = net.phase.empty() ? std::string{"network"} : net.phase;
    auto value_style = nxt::tui::fg(tt::slate_300) | nxt::tui::bg(tt::page_bg);
    auto phase_style = nxt::tui::fg(tt::teal_300) | nxt::tui::bg(tt::page_bg)
                     | nxt::tui::em(nxt::Emphasis::bold);
    auto event_style = nxt::tui::fg(tt::slate_500) | nxt::tui::bg(tt::page_bg);
    return nxt::tui::when(
        !net.phase.empty() || net.socket_rx != 0 || net.socket_tx != 0,
        nxt::tui::row(
            nxt::tui::hfill(1 * nxt::ch, tt::page_bg),
            fixed_cell(14 * nxt::ch, std::move(phase), phase_style),
            nxt::tui::hfill(1 * nxt::ch, tt::page_bg),
            fixed_cell(
                7 * nxt::ch,
                format_compact_bytes(net.socket_rx) + "↓",
                value_style,
                cell_align::right),
            nxt::tui::hfill(1 * nxt::ch, tt::page_bg),
            rate_cell(
                format_compact_rate(net.socket_rx_bps) + "↓",
                net.socket_rx_bps,
                tt::teal_300),
            nxt::tui::hfill(1 * nxt::ch, tt::page_bg),
            fixed_cell(
                7 * nxt::ch,
                format_compact_bytes(net.socket_tx) + "↑",
                value_style,
                cell_align::right),
            nxt::tui::hfill(1 * nxt::ch, tt::page_bg),
            rate_cell(
                format_compact_rate(net.socket_tx_bps) + "↑",
                net.socket_tx_bps,
                tt::amber_300),
            nxt::tui::hfill(1 * nxt::ch, tt::page_bg),
            fixed_cell(
                5 * nxt::ch,
                net.sse_events == 0
                    ? std::string{}
                    : std::format("#{}", net.sse_events),
                event_style,
                cell_align::right),
            nxt::tui::flex_text("", nxt::tui::bg(tt::page_bg))));
}

template<nxt::tui::Layout Child>
auto agent_layout(
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    Child && child)
{
    namespace tt = nxt::ai::tool_tui;
    return nxt::tui::surface(
        nxt::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxt::DEFAULT_EMPHASIS,
        },
        nxt::tui::column(
            header_layout(model, status),
            assistant_preview_layout(assistant_text),
            std::forward<Child>(child)));
}

auto stream_layout(
    std::string_view thought,
    std::string_view assistant_text,
    const network_hud_state & network)
{
    namespace tt = nxt::ai::tool_tui;
    return nxt::tui::surface(
        nxt::tui::Style{
            .fg = tt::slate_300,
            .bg = tt::page_bg,
            .em = nxt::DEFAULT_EMPHASIS,
        },
        nxt::tui::column(
            stream_activity_layout(thought, assistant_text),
            network_footer_layout(network)));
}

template<typename ToolSet>
nxt::rt::task<std::vector<nxt::ai::tools::function_call_result>>
run_function_tool_batch_ui(
    const ToolSet & tools,
    std::vector<nxt::ai::tools::function_call> calls,
    std::chrono::milliseconds settle_delay);

nxt::rt::task<void> publish_stream_view(
    std::string_view thought,
    std::string_view assistant_text,
    network_hud_state & network,
    nxt::rt::ema_rate & rx_rate,
    nxt::rt::ema_rate & tx_rate,
    std::size_t & last_rx,
    std::size_t & last_tx,
    std::chrono::steady_clock::time_point & last_sample,
    std::chrono::steady_clock::time_point & last_publish,
    bool force = false)
{
    if (!nxt::rt::has_terminal_surface())
        co_return;

    auto now = std::chrono::steady_clock::now();
    if (!force
        && last_publish != std::chrono::steady_clock::time_point{}
        && now - last_publish < frame_interval)
        co_return;

    auto next_rx = network.socket_rx;
    auto next_tx = network.socket_tx;
    network.socket_rx_bps =
        rx_rate.sample(next_rx >= last_rx ? next_rx - last_rx : 0,
                       now - last_sample);
    network.socket_tx_bps =
        tx_rate.sample(next_tx >= last_tx ? next_tx - last_tx : 0,
                       now - last_sample);
    last_rx = next_rx;
    last_tx = next_tx;
    last_sample = now;
    last_publish = now;
    co_await nxt::rt::draw(stream_layout(thought, assistant_text, network));
}

struct stream_view_publisher
{
    std::string & thought;
    std::string & assistant_text;
    network_hud_state & network;
    nxt::rt::ema_rate & rx_rate;
    nxt::rt::ema_rate & tx_rate;
    std::size_t last_rx;
    std::size_t last_tx;
    std::chrono::steady_clock::time_point last_sample;
    std::chrono::steady_clock::time_point last_publish{};
    bool pending = false;
    bool force_pending = false;

    void request(bool force = false) noexcept
    {
        pending = true;
        force_pending = force_pending || force;
    }

    nxt::rt::task<void> publish(bool force = false)
    {
        pending = false;
        force_pending = false;
        co_await publish_stream_view(
            thought,
            assistant_text,
            network,
            rx_rate,
            tx_rate,
            last_rx,
            last_tx,
            last_sample,
            last_publish,
            force);
    }

    nxt::rt::task<void> flush()
    {
        if (!pending)
            co_return;
        auto force = force_pending;
        pending = false;
        force_pending = false;
        co_await publish_stream_view(
            thought,
            assistant_text,
            network,
            rx_rate,
            tx_rate,
            last_rx,
            last_tx,
            last_sample,
            last_publish,
            force);
    }
};

stream_view_publisher make_stream_view_publisher(
    std::string & thought,
    std::string & assistant_text,
    network_hud_state & network,
    nxt::rt::ema_rate & rx_rate,
    nxt::rt::ema_rate & tx_rate)
{
    return stream_view_publisher{
        .thought = thought,
        .assistant_text = assistant_text,
        .network = network,
        .rx_rate = rx_rate,
        .tx_rate = tx_rate,
        .last_rx = network.socket_rx,
        .last_tx = network.socket_tx,
        .last_sample = std::chrono::steady_clock::now(),
    };
}

void note_tls_progress(
    const nxt::rt::tls::handshake_progress & progress,
    const std::shared_ptr<nxt::rt::trace_context> & trace,
    const nxt::rt::trace_span & tls_span,
    std::vector<nxt::rt::trace_span> & active_tls_spans,
    network_hud_state & network)
{
    if (progress.kind == nxt::rt::tls::handshake_progress_kind::begin) {
        network.phase = std::format(
            "TLS {}",
            nxt::ai::trace_tui::display_name(progress.name));
        if (trace != nullptr && tls_span)
            active_tls_spans.push_back(
                trace->start_span(
                    std::string{progress.name},
                    tls_span.span_id()));
        return;
    }

    if (progress.kind == nxt::rt::tls::handshake_progress_kind::end) {
        for (auto it = active_tls_spans.rbegin();
             it != active_tls_spans.rend();
             ++it) {
            if (it->name() == progress.name) {
                it->finish("ok");
                active_tls_spans.erase(std::next(it).base());
                break;
            }
        }
        return;
    }

    if (trace != nullptr && tls_span)
        tls_span.event(std::string{progress.name});
    network.phase = std::format(
        "TLS {}",
        nxt::ai::trace_tui::display_name(progress.name));
}

nxt::rt::task<void> print_tls_ready(
    const std::shared_ptr<nxt::rt::trace_context> & trace,
    const nxt::rt::trace_span & tls_span)
{
    if (!nxt::rt::has_terminal_surface())
        co_return;

    if (trace != nullptr && tls_span)
        nxt::rt::print(
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
        nxt::rt::print_block("tls  api.openai.com  TLS 1.3 handshake\n");
    co_return;
}

nxt::rt::task<stream_phase_result> stream_openai_response(
    const llm_request & request)
{
    auto thought = std::string{};
    auto assistant_text = std::string{};
    auto network = network_hud_state{.phase = "connecting"};
    auto rx_rate = nxt::rt::ema_rate{std::chrono::milliseconds{700}};
    auto tx_rate = nxt::rt::ema_rate{std::chrono::milliseconds{700}};
    auto publisher = make_stream_view_publisher(
        thought, assistant_text, network, rx_rate, tx_rate);
    co_await publisher.publish(true);

    auto socket = co_await nxt::rt::net::connect_tcp("api.openai.com", "443");
    network.phase = "tcp connected";
    co_await publisher.publish(true);
    auto socket_output = nxt::rt::byte_writer{
        nxt::rt::meter_sink(
            nxt::rt::socket_sink{socket.get()},
            [&](std::size_t bytes) {
                network.socket_tx += bytes;
                publisher.request();
            }),
        4096,
    };
    auto source = nxt::rt::meter_source(
        nxt::rt::socket_source{socket.get()},
        [&](std::size_t bytes) {
            network.socket_rx += bytes;
            publisher.request();
        });
    auto input_storage = std::vector<std::byte>(18 * 1024);
    auto reader = nxt::rt::byte_reader{
        source,
        std::span{input_storage},
    };

    auto tls = nxt::rt::tls::tls13_client_session{reader, socket_output};
    network.phase = "TLS handshake";
    co_await publisher.publish(true);
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
        auto progress = [&](
            const nxt::rt::tls::handshake_progress & step) {
            note_tls_progress(
                step, trace, tls_span, active_tls_spans, network);
            publisher.request(true);
        };
        co_await tls.handshake("api.openai.com", progress);
        co_await publisher.flush();
        if (tls_span)
            tls_span.finish("ok");
    } catch (...) {
        for (auto & span : active_tls_spans)
            span.finish("error");
        if (tls_span)
            tls_span.finish("error");
        throw;
    }
    co_await print_tls_ready(trace, tls_span);
    network.phase = "TLS ready";
    co_await publisher.publish(true);

    auto http_request =
        nxt::ai::responses::openai_responses_http_request(request);
    for (auto & header : http_request.headers) {
        if (header.name == "Connection")
            header.value = "close";
    }
    auto request_text = nxt::http::serialize(http_request);
    network.phase = "http request";
    co_await publisher.publish(true);
    co_await tls.write_all(request_text);
    co_await publisher.flush();

    auto http_storage = std::vector<std::byte>(18 * 1024);
    auto http_reader =
        nxt::rt::byte_reader{tls, std::span{http_storage}};
    network.phase = "http response";
    co_await publisher.publish(true);
    auto head = co_await nxt::rt::http::read_response_head(http_reader);
    co_await publisher.flush();
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
    network.phase = "streaming SSE";
    co_await publisher.publish(true);
    while (auto sse = co_await nxt::rt::http::parse_sse_event(sse_reader)) {
        co_await publisher.flush();
        if (sse->data == "[DONE]")
            break;
        ++network.sse_events;
        co_await publisher.publish();

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
            co_await publisher.publish(true);
        } else if (event.type == "response.reasoning_summary_text.delta") {
            auto delta =
                event.read<
                    nxt::ai::openai::reasoning_summary_text_delta_event>();
            append_thought_delta(thought, delta.delta);
            co_await publisher.publish();
        } else if (event.type == "response.reasoning_summary_text.done") {
            auto done =
                event.read<
                    nxt::ai::openai::reasoning_summary_text_done_event>();
            if (!done.text.empty())
                thought = std::move(done.text);
            auto summary = std::move(thought);
            thought.clear();
            if (nxt::rt::has_terminal_surface() && !summary.empty())
                nxt::rt::print(
                    nxt::ai::tool_tui::thought_block(std::move(summary)));
            co_await publisher.publish(true);
        } else if (event.type == "response.output_text.delta") {
            auto delta = event.read<nxt::ai::openai::text_delta_event>();
            auto first_stream_delta = assistant_text.empty();
            auto text = std::move(delta.delta);
            assistant_text += text;
            if (nxt::rt::has_terminal_surface()) {
                co_await publisher.publish(first_stream_delta);
            } else {
                co_await nxt::rt::write_stdout_all(std::move(text));
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

    if (!nxt::rt::has_terminal_surface())
        co_await nxt::rt::write_stdout_all("\n");
    co_return stream_phase_result{
        .response = std::move(result),
        .assistant_text = std::move(assistant_text),
    };
}

template<typename T>
T take_phase_result(nxt::rt::catching_deed<T> deed)
{
    auto result = std::move(deed).get();
    if (result)
        return std::move(*result);
    nxt::rt::rethrow(result.error());
}

nxt::rt::task<nxt::rt::catching_deed<stream_phase_result>>
spawn_stream_phase_child(
    const llm_request & request,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text)
{
    auto child = nxt::rt::spawn_widget(
        [&request] {
            return stream_openai_response(request);
        });
    co_await nxt::rt::draw(
        agent_layout(
            model, status, assistant_text, child.surface()));
    co_return std::move(child).cope();
}

nxt::rt::task<stream_phase_result> run_stream_phase(
    const llm_request & request,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text)
{
    auto deed = co_await nxt::rt::with_zone(
        [&] {
            return spawn_stream_phase_child(
                request,
                model,
                status,
                assistant_text);
        });
    co_return take_phase_result(std::move(deed));
}

template<typename ToolSet>
nxt::rt::task<nxt::rt::catching_deed<std::vector<function_call_result>>>
spawn_tool_phase_child(
    const ToolSet & tools,
    std::vector<function_call> calls,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    std::chrono::milliseconds settle_delay)
{
    auto child = nxt::rt::spawn_widget(
        [&tools,
         calls = std::move(calls),
         settle_delay]() mutable {
            return run_function_tool_batch_ui(
                tools,
                std::move(calls),
                settle_delay);
        });
    co_await nxt::rt::draw(
        agent_layout(
            model, status, assistant_text, child.surface()));
    co_return std::move(child).cope();
}

template<typename ToolSet>
nxt::rt::task<std::vector<nxt::ai::tools::function_call_result>>
run_tool_phase(
    const ToolSet & tools,
    std::vector<nxt::ai::tools::function_call> calls,
    std::string_view model,
    std::string_view status,
    std::string_view assistant_text,
    std::chrono::milliseconds settle_delay)
{
    auto deed = co_await nxt::rt::with_zone(
        [&]() mutable {
            return spawn_tool_phase_child(
                tools,
                std::move(calls),
                model,
                status,
                assistant_text,
                settle_delay);
        });
    co_return take_phase_result(std::move(deed));
}

void print_assistant_if_terminal(std::string & assistant_text)
{
    if (!nxt::rt::has_terminal_surface() || assistant_text.empty())
        return;
    nxt::rt::print(
        nxt::ai::tool_tui::assistant_block(
            std::move(assistant_text)));
    assistant_text.clear();
}

template<typename ToolSet>
nxt::rt::task<void> run_agent_loop(
    llm_request request,
    ToolSet tools,
    std::size_t max_steps = 32)
{
    auto original = request;
    auto stateless_input = nxt::ai::responses::input_items_from_request(request);
    auto assistant_text = std::string{};
    auto status = std::string{"starting"};
    auto model = request.model;
    auto settle_delay = nxt::rt::has_terminal_surface()
        ? std::chrono::milliseconds{900}
        : std::chrono::milliseconds{0};
    prepare_tool_request(request, tools);
    co_await nxt::rt::draw(
        agent_layout(model, status, assistant_text, nxt::tui::empty()));

    for (std::size_t step = 0; step < max_steps; ++step) {
        status = std::format("turn {} streaming", step + 1);
        auto stream = co_await run_stream_phase(
            request, model, status, assistant_text);
        auto response = std::move(stream.response);
        assistant_text += stream.assistant_text;
        auto calls =
            nxt::ai::tools::function_calls_from_items(response.output_items);
        if (calls.empty()) {
            print_assistant_if_terminal(assistant_text);
            status = "done";
            co_await nxt::rt::draw(
                agent_layout(
                    model, status, assistant_text, nxt::tui::empty()));
            co_return;
        }

        if (request.store && !response.response_id)
            throw nxt::rt::runtime_error{
                "tool call response had no response id"};

        auto started = std::chrono::steady_clock::now();
        status = std::format("running {} tool call(s)", calls.size());

        auto results = co_await run_tool_phase(
            tools,
            std::move(calls),
            model,
            status,
            assistant_text,
            settle_delay);
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        status = std::format(
            "{} tool call(s) in {}ms", results.size(), elapsed);
        co_await nxt::rt::draw(
            agent_layout(model, status, assistant_text, nxt::tui::empty()));

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

template<typename ToolSet>
nxt::rt::task<nxt::ai::tools::tool_result> run_one_tool_call_worker(
    const ToolSet & tools,
    const nxt::ai::tools::function_call & call,
    bool & done)
{
    try {
        auto result = co_await nxt::ai::tools::run_function_tool(tools, call);
        done = true;
        co_return result;
    } catch (...) {
        done = true;
        throw;
    }
}

template<typename ToolSet>
nxt::rt::task<nxt::rt::catching_deed<tool_result>>
run_one_tool_call_meter(
    const ToolSet & tools,
    const function_call & call,
    std::chrono::steady_clock::time_point started,
    nxt::ai::tool_tui::call_view & view,
    bool & done)
{
    auto deed = nxt::rt::fork(
        run_one_tool_call_worker(tools, call, done)).cope();

    while (!done) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        view.elapsed_ms = static_cast<int>(elapsed);
        co_await nxt::rt::draw(nxt::ai::tool_tui::render_call(view));
        co_await nxt::rt::op::timeout::after(frame_interval);
    }
    co_return std::move(deed);
}

template<typename ToolSet>
nxt::rt::task<nxt::ai::tools::function_call_result> run_one_tool_call_ui(
    const ToolSet & tools,
    nxt::ai::tools::function_call call,
    std::chrono::milliseconds settle_delay)
{
    auto started = std::chrono::steady_clock::now();
    auto view = nxt::ai::tool_tui::call_view{
        .name = call.name,
        .arguments = call.arguments,
        .output = {},
        .observed = std::nullopt,
        .state = nxt::ai::tool_tui::status::running,
        .elapsed_ms = -1,
    };
    co_await nxt::rt::draw(nxt::ai::tool_tui::render_call(view));

    auto done = false;
    auto worker = co_await nxt::rt::with_zone(
        [&] {
            return run_one_tool_call_meter(
                tools,
                call,
                started,
                view,
                done);
        });

    auto finished = std::move(worker).get();
    if (!finished)
        nxt::rt::rethrow(finished.error());
    auto result = std::move(*finished);
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count();

    view.state = result.failed
        ? nxt::ai::tool_tui::status::error
        : nxt::ai::tool_tui::status::ok;
    view.output = result.output;
    view.observed = result.observed;
    view.elapsed_ms = static_cast<int>(elapsed);
    co_await nxt::rt::draw(nxt::ai::tool_tui::render_call(view));
    if (settle_delay > std::chrono::milliseconds{0})
        co_await nxt::rt::op::timeout::after(settle_delay);

    auto output_item =
        nxt::ai::tools::function_call_output(
            call.call_id,
            nxt::ai::tools::tool_result_json(result));
    co_return nxt::ai::tools::function_call_result{
        .call = std::move(call),
        .result = std::move(result),
        .output_item = std::move(output_item),
    };
}

template<typename ToolSet>
nxt::rt::task<nxt::ai::tools::function_call_result>
run_one_tool_call_counted(
    const ToolSet & tools,
    nxt::ai::tools::function_call call,
    std::chrono::milliseconds settle_delay,
    std::size_t & done_count)
{
    try {
        auto result = co_await run_one_tool_call_ui(
            tools,
            std::move(call),
            settle_delay);
        ++done_count;
        co_return result;
    } catch (...) {
        ++done_count;
        throw;
    }
}

template<typename ToolSet>
nxt::rt::task<std::vector<nxt::rt::catching_deed<function_call_result>>>
spawn_tool_call_children(
    const ToolSet & tools,
    std::vector<function_call> & calls,
    std::vector<nxt::rt::widget_slot> & surfaces,
    std::chrono::milliseconds settle_delay,
    std::size_t & done_count)
{
    auto out = std::vector<nxt::rt::catching_deed<function_call_result>>{};
    out.reserve(calls.size());

    surfaces.reserve(calls.size());
    for (auto & call : calls) {
        auto child = nxt::rt::spawn_widget(
            [&tools,
             call = std::move(call),
             settle_delay,
             &done_count]() mutable {
                return run_one_tool_call_counted(
                    tools,
                    std::move(call),
                    settle_delay,
                    done_count);
            });
        surfaces.emplace_back(child.surface());
        out.push_back(std::move(child).cope());
    }
    co_await nxt::rt::draw(nxt::rt::child_slots_column(surfaces));

    while (done_count < out.size())
        co_await nxt::rt::op::timeout::after(frame_interval);
    co_return out;
}

template<typename ToolSet>
nxt::rt::task<std::vector<nxt::ai::tools::function_call_result>>
run_function_tool_batch_ui(
    const ToolSet & tools,
    std::vector<nxt::ai::tools::function_call> calls,
    std::chrono::milliseconds settle_delay)
{
    if (!nxt::rt::has_terminal_surface()) {
        co_await nxt::rt::write_stdout_all(
            std::format(
                "[tools] running {} tool call(s)\n", calls.size()));
        auto results = co_await nxt::ai::tools::run_function_tool_batch(
            tools,
            std::move(calls));
        for (const auto & result : results) {
            co_await nxt::rt::write_stdout_all(
                std::format(
                    "[tool] {} -> {} ({} bytes)\n",
                    result.call.name,
                    result.result.failed ? "failed" : "ok",
                    result.result.output.size()));
        }
        co_return results;
    }

    auto done_count = std::size_t{};
    auto surfaces = std::vector<nxt::rt::widget_slot>{};
    auto deeds = co_await nxt::rt::with_zone(
        [&] {
            return spawn_tool_call_children(
                tools,
                calls,
                surfaces,
                settle_delay,
                done_count);
        });

    try {
        auto out = std::vector<nxt::ai::tools::function_call_result>{};
        out.reserve(deeds.size());
        for (auto & deed : deeds) {
            auto result = std::move(deed).get();
            if (result) {
                out.push_back(std::move(*result));
            } else {
                nxt::rt::rethrow(result.error());
            }
        }
        nxt::rt::clear_widget();
        co_return out;
    } catch (...) {
        nxt::rt::clear_widget();
        throw;
    }
}

template<typename ToolSet>
nxt::rt::task<void> run_agent_ui_zone(
    llm_request request,
    ToolSet tools)
{
    try {
        co_await run_agent_loop(
            std::move(request),
            std::move(tools));
        nxt::rt::request_ui_shutdown();
    } catch (...) {
        nxt::rt::request_ui_shutdown();
        throw;
    }
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

    auto trace = std::make_shared<nxt::rt::trace_context>();
    auto root_span = trace->start_span(
        "nxtllm.request",
        {},
        {{"model", request.model}});

    try {
        co_await nxt::rt::with_env<nxt::rt::trace_context_key>(
            trace,
            [&]() mutable {
                return nxt::rt::with_env<nxt::rt::trace_current_span_key>(
                    root_span.span_id(),
                    [&]() mutable {
                        return nxt::rt::with_ui_zone(
                            [request = std::move(request),
                             tools = std::move(tools)]() mutable {
                                return run_agent_ui_zone(
                                    std::move(request),
                                    std::move(tools));
                            },
                            {},
                            frame_interval);
                    });
            });
        root_span.finish("ok");
    } catch (...) {
        root_span.finish("error");
        throw;
    }

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
