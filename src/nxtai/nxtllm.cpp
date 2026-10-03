#include <nxtrt/app.hpp>
#include <nxtrt/buffers.hpp>
#include <nxtrt/http.hpp>
#include <nxtrt/net_dns.hpp>
#include <nxtrt/tls.hpp>
#include <nxt/stacktrace.hpp>
#include <nxtai/agent.hpp>
#include <nxtai/agent_tools.hpp>
#include <nxtai/tool_json.hpp>

#include <array>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr auto openai_sse_body_buffer_size = std::size_t{1024 * 1024};

struct cli_options
{
    std::string model = "gpt-6-luna";
    std::size_t max_output_tokens = 20000;
    bool store = false;
    bool tools = true;
    std::size_t max_turns = 32;
    bool dump_request = false;
    std::optional<std::string> prompt;
};

struct missing_prompt : nxtrt::runtime_error
{
    missing_prompt()
        : nxtrt::runtime_error{"missing prompt"}
    {
    }
};

struct openai_http_error : nxtrt::runtime_error
{
    openai_http_error(int status, std::string reason)
        : nxtrt::runtime_error{"OpenAI Responses HTTP error"}
        , status(status)
        , reason(std::move(reason))
    {
    }

    int status = 0;
    std::string reason;
};

struct openai_unexpected_content_type : nxtrt::runtime_error
{
    openai_unexpected_content_type(
        std::string expected, std::optional<std::string> actual)
        : nxtrt::runtime_error{"OpenAI Responses unexpected content-type"}
        , expected(std::move(expected))
        , actual(std::move(actual))
    {
    }

    std::string expected;
    std::optional<std::string> actual;
};

[[noreturn]] void print_help_and_exit()
{
    std::cout
        << "usage: nxtllm [options] [prompt...]\n"
           "  streams OpenAI Responses and runs local tools over nxtrt\n"
           "\n"
           "  -m, --model MODEL                 (default: gpt-6-luna)\n"
           "  --max-output-tokens N\n"
           "  --max-turns N                     (default: 32)\n"
           "  --no-tools                        disable local file/search/shell tools\n"
           "  --store                           ask OpenAI to store the response\n"
           "  --dump-request                    print serialized Responses JSON\n";
    std::exit(EXIT_SUCCESS);
}

std::size_t parse_size(std::string_view text, std::string_view name)
{
    auto value = std::size_t{};
    auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size())
        throw nxtrt::runtime_error{"invalid " + std::string{name}};
    return value;
}

cli_options parse_args(int argc, char ** argv)
{
    auto options = cli_options{};
    auto positionals = std::vector<std::string_view>{};

    for (auto i = 1; i < argc; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--model" || arg == "-m") {
            if (++i >= argc)
                throw nxtrt::runtime_error{"--model needs a value"};
            options.model = argv[i];
        } else if (arg == "--max-output-tokens") {
            if (++i >= argc)
                throw nxtrt::runtime_error{
                    "--max-output-tokens needs a value"};
            options.max_output_tokens =
                parse_size(argv[i], "--max-output-tokens");
        } else if (arg == "--no-tools") {
            options.tools = false;
        } else if (arg == "--max-turns") {
            if (++i >= argc)
                throw nxtrt::runtime_error{"--max-turns needs a value"};
            options.max_turns = parse_size(argv[i], "--max-turns");
            if (options.max_turns == 0)
                throw nxtrt::runtime_error{"--max-turns must be nonzero"};
        } else if (arg == "--store") {
            options.store = true;
        } else if (arg == "--dump-request") {
            options.dump_request = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help_and_exit();
        } else {
            positionals.push_back(arg);
        }
    }

    if (!positionals.empty()) {
        auto prompt = std::string{};
        for (auto part : positionals) {
            if (!prompt.empty())
                prompt.push_back(' ');
            prompt += part;
        }
        options.prompt = std::move(prompt);
    }

    return options;
}

std::string env_string(const char * name)
{
    auto * value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

nxtrt::task<std::string> read_env_string(const char * name)
{
    co_return env_string(name);
}

std::string nxtllm_accept_encoding_header()
{
    auto value = std::string{"gzip, deflate"};
#if defined(NXTRT_HAVE_ZSTD)
    value += ", zstd";
#endif
#if defined(NXTRT_HAVE_BROTLI)
    value += ", br";
#endif
    return value;
}

nxtai::responses::openai_responses_request
make_request(const cli_options & options, std::string api_key)
{
    return nxtai::responses::openai_responses_request{
        .api_key = std::move(api_key),
        .model = options.model,
        .input = options.prompt.value_or(""),
        .max_output_tokens = options.max_output_tokens,
        .reasoning_effort = {},
        .reasoning_summary = {},
        .store = options.store,
    };
}

nxtrt::http::request
openai_request(const nxtai::responses::openai_responses_request & request)
{
    return nxtrt::http::request{
        .method = "POST",
        .target = "/v1/responses",
        .host = "api.openai.com",
        .headers =
            {
                {"User-Agent", "nxtllm/0"},
                {"Accept", "text/event-stream"},
                {"Accept-Encoding", nxtllm_accept_encoding_header()},
                {"Content-Type", "application/json"},
                {"Authorization", "Bearer " + request.api_key},
                {"Connection", "close"},
            },
        .body = nxtai::responses::openai_responses_body(request),
    };
}

bool response_status_is_success(const nxtrt::http::response_head & head)
{
    return head.status >= 200 && head.status < 300;
}

std::optional<std::string>
response_content_media_type(const nxtrt::http::response_head & head)
{
    auto value = nxtrt::http::header_value(head, "content-type");
    if (!value)
        return std::nullopt;
    auto semicolon = value->find(';');
    auto media_type = nxtrt::http::trim_ascii(value->substr(0, semicolon));
    return std::string{media_type};
}

nxtrt::task<nxtrt::http::response_head> open_response_stream(
    nxtrt::tls::tls13_client_session & tls, std::string_view request_text)
{
    co_await tls.handshake("api.openai.com");
    co_await tls.write_all(request_text);

    auto head = co_await nxtrt::http::read_response_head(tls);
    if (!response_status_is_success(head))
        throw openai_http_error{head.status, head.reason};

    auto media_type = response_content_media_type(head);
    if (!media_type
        || !nxtrt::http::iequals(*media_type, "text/event-stream"))
        throw openai_unexpected_content_type{
            "text/event-stream",
            std::move(media_type),
        };

    co_return head;
}

struct console_observer
{
    nxtrt::bytesink & output;

    nxtrt::task<void> text(std::string delta)
    {
        co_await nxtrt::write_all(output, delta);
    }

    nxtrt::task<void> tool_started(const nxtai::tools::function_call & call)
    {
        std::cerr << "\n[tool " << call.name << " " << call.call_id << "]\n";
        co_return;
    }

    nxtrt::task<void> tool_finished(const nxtai::tools::function_call_result & result)
    {
        std::cerr << "[tool " << result.call.name
                  << (result.result.failed ? " failed" : " done") << "]\n";
        co_return;
    }
};

struct stream_request
{
    nxtai::responses::openai_responses_request request;
    console_observer & observer;
    std::array<std::byte, 16 * 1024> socktxbuf{};
    std::array<std::byte, 64 * 1024> sockrxbuf{};
    std::array<std::byte, 64 * 1024> tlsbuf{};

    nxtrt::task<nxtai::responses::response_result> operator()()
    {
        return stream_openai_response();
    }

private:
    nxtrt::task<nxtai::responses::response_result> stream_openai_response()
    {
        auto request_text = nxtrt::http::serialize(openai_request(request));

        auto socket = nxtrt::net::socket{
            co_await nxtrt::net::connect_tcp("api.openai.com", "443"),
            std::span{socktxbuf},
            std::span{sockrxbuf},
        };

        auto tls = nxtrt::tls::tls13_client_session{
            socket,
            std::span{tlsbuf},
        };

        auto head = co_await open_response_stream(tls, request_text);

        auto body = nxtrt::http::response_body_decoding_reader{
            tls,
            head,
            openai_sse_body_buffer_size};
        auto events = nxtrt::http::sse_event_parser(body);

        auto decoder = nxtai::responses::stream_decoder{};
        while (auto event = co_await events.take()) {
            if (auto delta = co_await decoder.accept(event->type, event->data))
                co_await observer.text(std::move(*delta));
            if (decoder.completed)
                co_return decoder.finish();
        }
        co_return decoder.finish();
    }
};

struct openai_transport
{
    nxtrt::task<nxtai::responses::response_result> operator()(
        const nxtai::responses::openai_responses_request & request,
        console_observer & observer)
    {
        auto stream = stream_request{.request = request, .observer = observer};
        co_return co_await stream();
    }
};

nxtrt::task<int> run_nxtllm(cli_options options)
{
    auto output = nxtrt::standard_output_sink(64 * 1024);

    if (!options.prompt) {
        throw missing_prompt{};
    }

    auto request =
        make_request(options, co_await read_env_string("OPENAI_API_KEY"));

    auto tools = options.tools ? nxtai::agent_tools::for_agent()
                               : nxtai::tools::tool_registry{};
    request.tools = nxtai::tools::function_tool_definitions(tools);
    if (options.tools) {
        if (!request.store)
            request.include = {"reasoning.encrypted_content"};
        auto context = nxt::json::writer{};
        context.raw("{\"role\":\"developer\",\"content\":");
        context.string("You are a local coding assistant. Use the available tools "
                       "to inspect files before answering questions about this repository. "
                       "Working directory: " + std::filesystem::current_path().string());
        context.character('}');
        request.input_items = {{std::move(context.out)}};
        auto user_request = nxtai::responses::openai_responses_request{};
        user_request.input = request.input;
        for (auto & item : nxtai::responses::input_items_from_request(user_request))
            request.input_items.push_back(std::move(item));
    }

    if (options.dump_request) {
        co_await nxtrt::print_all(output,
            "{}\n", nxtai::responses::openai_responses_body(request));
        co_return EXIT_SUCCESS;
    }

    if (request.api_key.empty()) {
        throw nxtrt::runtime_error{"OPENAI_API_KEY is not set"};
    }

    auto observer = console_observer{output};
    auto transport = openai_transport{};
    co_await nxtai::run_agent(std::move(request), tools, transport, observer,
        {.max_turns = options.max_turns});
    co_await nxtrt::write_all(output, "\n");

    co_return EXIT_SUCCESS;
}

} // namespace

std::string_view exception_message(const std::exception & error)
{
#ifdef NXT_HAVE_CPPTRACE
    if (auto traced = dynamic_cast<const nxt::debug::exception *>(&error))
        return traced->message();
#endif
    return error.what();
}

int report_exception(const missing_prompt &)
{
    std::cout
        << "Pass a prompt, or use --dump-request with a prompt to inspect "
           "the JSON envelope.\n";
    return EXIT_SUCCESS;
}

int report_exception(const openai_http_error & error)
{
    std::cerr << "nxtllm: OpenAI Responses HTTP error: " << error.status;
    if (!error.reason.empty())
        std::cerr << ' ' << error.reason;
    std::cerr << '\n';
    nxt::debug::print_current_exception_trace(std::cerr, "  ");
    return EXIT_FAILURE;
}

int report_exception(const openai_unexpected_content_type & error)
{
    std::cerr << "nxtllm: OpenAI Responses expected " << error.expected;
    if (error.actual)
        std::cerr << ", got " << *error.actual;
    else
        std::cerr << ", got no content-type";
    std::cerr << '\n';
    nxt::debug::print_current_exception_trace(std::cerr, "  ");
    return EXIT_FAILURE;
}

int report_exception(const std::exception & error)
{
    std::cerr << "nxtllm: " << exception_message(error) << '\n';
    nxt::debug::print_current_exception_trace(std::cerr, "  ");
    return EXIT_FAILURE;
}

int report_unknown_exception()
{
    std::cerr << "nxtllm: unknown exception\n";
    nxt::debug::print_current_exception_trace(std::cerr, "  ");
    return EXIT_FAILURE;
}

int nxtllm_main(int argc, char ** argv)
{
    auto exit_code = EXIT_FAILURE;
    nxt::debug::try_catch(
        [&] {
            auto rt = nxtrt::runtime{};
            exit_code = rt.run([&] {
                return run_nxtllm(parse_args(argc, argv));
            });
        },
        [&](const missing_prompt & e) { exit_code = report_exception(e); },
        [&](const openai_http_error & e) {
            exit_code = report_exception(e);
        },
        [&](const openai_unexpected_content_type & e) {
            exit_code = report_exception(e);
        },
        [&](const std::exception & e) { exit_code = report_exception(e); },
        [&] { exit_code = report_unknown_exception(); });
    return exit_code;
}

#if !defined(NXT_EMBEDDED_MAIN)
int main(int argc, char ** argv)
{
    return nxtllm_main(argc, argv);
}
#endif
