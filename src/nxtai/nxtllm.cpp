#include <nxtrt/app.hpp>
#include <nxtrt/buffers.hpp>
#include <nxtrt/http.hpp>
#include <nxtrt/net_dns.hpp>
#include <nxtrt/tls.hpp>
#include <nxt/stacktrace.hpp>
#include <nxtai/responses_request.hpp>
#include <nxtai/tool_json.hpp>

#include <array>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct cli_options
{
    std::string model = "gpt-5.4-mini";
    std::size_t max_output_tokens = 20000;
    bool store = false;
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

struct openai_terminal_event : nxtrt::runtime_error
{
    explicit openai_terminal_event(std::string type)
        : nxtrt::runtime_error{"OpenAI Responses terminal event"}
        , type(std::move(type))
    {
    }

    std::string type;
};

struct openai_unexpected_event : nxtrt::runtime_error
{
    openai_unexpected_event(std::string expected, std::string actual)
        : nxtrt::runtime_error{"OpenAI Responses unexpected event"}
        , expected(std::move(expected))
        , actual(std::move(actual))
    {
    }

    std::string expected;
    std::string actual;
};

[[noreturn]] void print_help_and_exit()
{
    std::cout
        << "usage: nxtllm [options] [prompt...]\n"
           "  streams one OpenAI Responses request over nxtrt\n"
           "\n"
           "  -m, --model MODEL                 (default: gpt-5-mini)\n"
           "  --max-output-tokens N\n"
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

std::string accept_encoding_header()
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
                {"Accept-Encoding", accept_encoding_header()},
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
    nxtrt::tls::tls13_client_session & tls,
    std::string_view request_text)
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

nxtrt::task<void> write_sse_event_debug(
    nxtrt::byte_writer & output,
    const nxtrt::http::server_sent_event & event)
{
    co_await output.print_all("[{}]\n{}\n\n", event.type, event.data);
}

struct openai_response_stream_client
{
    nxtrt::byte_reader & body;
    nxtrt::byte_writer & output;
    std::optional<nxtrt::http::server_sent_event> pending = std::nullopt;

    nxtrt::task<void> stream_response()
    {
        co_await output.write_all("[response]\n");
        co_await write_sse_event_debug(
            output,
            co_await expect_event("response.created"));

        while (true) {
            auto event = co_await next_event();
            fail_on_terminal_event(event);

            if (event.type == "response.in_progress") {
                co_await write_sse_event_debug(output, event);
            } else if (event.type == "response.output_item.added") {
                pending = std::move(event);
                co_await stream_output_item();
            } else if (event.type == "response.completed") {
                co_await write_sse_event_debug(output, event);
                break;
            } else {
                co_await write_sse_event_debug(output, event);
            }
        }

        co_await output.write_all("[/response]\n");
    }

private:
    nxtrt::task<nxtrt::http::server_sent_event> next_event()
    {
        if (pending) {
            auto event = std::move(*pending);
            pending.reset();
            co_return event;
        }

        auto event = co_await nxtrt::http::parse_sse_event(body);
        if (!event)
            throw openai_unexpected_event{"SSE event", "end of stream"};
        if (event->data == "[DONE]")
            throw openai_unexpected_event{"response.completed", "[DONE]"};
        co_return std::move(*event);
    }

    nxtrt::task<nxtrt::http::server_sent_event> expect_event(
        std::string_view type)
    {
        auto event = co_await next_event();
        fail_on_terminal_event(event);
        if (event.type != type)
            throw openai_unexpected_event{
                std::string{type},
                std::move(event.type),
            };
        co_return event;
    }

    static void fail_on_terminal_event(
        const nxtrt::http::server_sent_event & event)
    {
        if (event.type == "response.failed"
            || event.type == "response.incomplete")
            throw openai_terminal_event{event.type};
    }

    nxtrt::task<void> stream_output_item()
    {
        co_await output.write_all("[response.output_item]\n");
        co_await write_sse_event_debug(
            output,
            co_await expect_event("response.output_item.added"));

        while (true) {
            auto event = co_await next_event();
            fail_on_terminal_event(event);

            if (event.type == "response.content_part.added") {
                pending = std::move(event);
                co_await stream_content_part();
            } else if (event.type == "response.output_item.done") {
                co_await write_sse_event_debug(output, event);
                break;
            } else {
                co_await write_sse_event_debug(output, event);
            }
        }

        co_await output.write_all("[/response.output_item]\n");
    }

    nxtrt::task<void> stream_content_part()
    {
        co_await output.write_all("[response.content_part]\n");
        co_await write_sse_event_debug(
            output,
            co_await expect_event("response.content_part.added"));

        co_await stream_output_text();

        co_await write_sse_event_debug(
            output,
            co_await expect_event("response.content_part.done"));
        co_await output.write_all("[/response.content_part]\n");
    }

    nxtrt::task<void> stream_output_text()
    {
        co_await output.write_all("[response.output_text]\n");

        while (true) {
            auto event = co_await next_event();
            fail_on_terminal_event(event);

            if (event.type == "response.output_text.delta") {
                auto text =
                    nxtai::tools::json_string_member(event.data, "delta")
                        .value_or(std::string{});
                co_await output.print_all("<<{}>>\n", text);
            } else if (event.type == "response.output_text.done") {
                co_await write_sse_event_debug(output, event);
                break;
            } else {
                pending = std::move(event);
                break;
            }
        }

        co_await output.write_all("[/response.output_text]\n");
    }
};

struct stream_zone_body
{
    nxtai::responses::openai_responses_request request;
    nxtrt::byte_writer & output;
    std::array<std::byte, 16 * 1024> socktxbuf{};
    std::array<std::byte, 64 * 1024> sockrxbuf{};
    std::array<std::byte, 64 * 1024> tlsbuf{};

    nxtrt::task<void> operator()()
    {
        return stream_openai_response();
    }

private:
    nxtrt::task<void> stream_openai_response()
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

        auto body = nxtrt::http::read_response_body(tls, head);
        auto client = openai_response_stream_client{body, output};
        co_await client.stream_response();

        co_await output.write_all("\n");
    }
};

nxtrt::task<int> run_nxtllm(cli_options options)
{
    auto output = nxtrt::standard_output_writer(64 * 1024);

    if (!options.prompt) {
        throw missing_prompt{};
    }

    auto request = make_request(
        options,
        co_await read_env_string("OPENAI_API_KEY"));

    if (options.dump_request) {
        co_await output.print_all(
            "{}\n", nxtai::responses::openai_responses_body(request));
        co_return EXIT_SUCCESS;
    }

    if (request.api_key.empty()) {
        throw nxtrt::runtime_error{"OPENAI_API_KEY is not set"};
    }

    co_await nxtrt::with_zone(
        stream_zone_body{
            .request = std::move(request),
            .output = output,
        });

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

int report_exception(const openai_terminal_event & error)
{
    std::cerr << "nxtllm: OpenAI Responses terminal event: " << error.type
              << '\n';
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

int main(int argc, char ** argv)
{
    auto exit_code = EXIT_FAILURE;
    nxt::debug::try_catch(
        [&] {
            auto rt = nxtrt::runtime{};
            exit_code = rt.run(run_nxtllm(parse_args(argc, argv)));
        },
        [&](const missing_prompt & e) { exit_code = report_exception(e); },
        [&](const openai_http_error & e) { exit_code = report_exception(e); },
        [&](const openai_unexpected_content_type & e) {
            exit_code = report_exception(e);
        },
        [&](const openai_terminal_event & e) {
            exit_code = report_exception(e);
        },
        [&](const std::exception & e) { exit_code = report_exception(e); },
        [&] { exit_code = report_unknown_exception(); });
    return exit_code;
}
