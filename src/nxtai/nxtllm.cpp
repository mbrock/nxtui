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

namespace openai_response_scope {

inline constexpr auto response = std::string_view{"response"};
inline constexpr auto output_item =
    std::string_view{"response.output_item"};
inline constexpr auto content_part =
    std::string_view{"response.content_part"};
inline constexpr auto output_text =
    std::string_view{"response.output_text"};

} // namespace openai_response_scope

namespace openai_response_event {

inline constexpr auto response_created =
    std::string_view{"response.created"};
inline constexpr auto response_in_progress =
    std::string_view{"response.in_progress"};
inline constexpr auto response_completed =
    std::string_view{"response.completed"};
inline constexpr auto output_item_added =
    std::string_view{"response.output_item.added"};
inline constexpr auto output_item_done =
    std::string_view{"response.output_item.done"};
inline constexpr auto content_part_added =
    std::string_view{"response.content_part.added"};
inline constexpr auto content_part_done =
    std::string_view{"response.content_part.done"};
inline constexpr auto output_text_delta =
    std::string_view{"response.output_text.delta"};
inline constexpr auto output_text_done =
    std::string_view{"response.output_text.done"};

} // namespace openai_response_event

using breadcrumb = std::vector<std::string_view>;

struct breadcrumb_key
{
    using value_type = breadcrumb;
    static constexpr auto name = "breadcrumb";
};

breadcrumb current_breadcrumb()
{
    if (auto value = nxtrt::env_get<breadcrumb_key>())
        return *value;
    return {};
}

std::size_t current_breadcrumb_depth()
{
    if (auto value = nxtrt::env_get<breadcrumb_key>())
        return value->size();
    return 0;
}

nxtrt::task<void>
write_indent(nxtrt::byte_writer & output, std::size_t depth)
{
    co_await output.write_splat("  ", depth);
}

nxtrt::task<void> write_indented_line(
    nxtrt::byte_writer & output, std::size_t depth, std::string_view text)
{
    co_await write_indent(output, depth);
    co_await output.write(text);
    co_await output.write(std::string_view{"\n"});
}

nxtrt::task<void> write_indented_block(
    nxtrt::byte_writer & output, std::size_t depth, std::string_view text)
{
    auto offset = std::size_t{0};
    while (offset < text.size()) {
        auto newline = text.find('\n', offset);
        auto line_end =
            newline == std::string_view::npos ? text.size() : newline;

        co_await write_indented_line(
            output, depth, text.substr(offset, line_end - offset));

        if (newline == std::string_view::npos)
            co_return;
        offset = newline + 1;
    }

    if (text.empty())
        co_await write_indented_line(output, depth, {});
}

nxtrt::task<void> write_sse_event_debug(
    nxtrt::byte_writer & output,
    nxtrt::http::server_sent_event event,
    std::size_t depth)
{
    co_await write_indent(output, depth);
    co_await output.print_all("* {}\n", event.type);
    co_await write_indented_block(output, depth, event.data);
    co_await output.write_all("\n");
}

struct breadcrumb_output
{
    nxtrt::byte_writer & output;
    breadcrumb rendered = {};

    nxtrt::task<void> render_current()
    {
        co_await render(current_breadcrumb());
    }

    nxtrt::task<void> close_all()
    {
        co_await render({});
    }

    nxtrt::task<void> write_event(nxtrt::http::server_sent_event event)
    {
        co_await render_current();
        if (false)
            co_await write_sse_event_debug(
                output, std::move(event), current_breadcrumb_depth());
    }

    nxtrt::task<void> write_text_delta(std::string text)
    {
        co_await render_current();
        co_await write_indent(output, current_breadcrumb_depth());
        co_await output.print_all("<<{}>>\n", text);
    }

private:
    nxtrt::task<void> render(breadcrumb next)
    {
        auto keep = std::size_t{0};
        while (keep < rendered.size() && keep < next.size()
               && rendered[keep] == next[keep])
            ++keep;

        for (auto i = rendered.size(); i > keep; --i)
            co_await write_marker(i - 1, rendered[i - 1], true);

        for (auto i = keep; i < next.size(); ++i)
            co_await write_marker(i, next[i], false);

        rendered = std::move(next);
    }

    nxtrt::task<void>
    write_marker(std::size_t depth, std::string_view name, bool closing)
    {
        co_await write_indent(output, depth);
        if (closing)
            co_await output.print("[/{}]\n", name);
        else
            co_await output.print("[{}]\n", name);
    }
};

template<typename Fn>
nxtrt::task<void> with_breadcrumb_scope(std::string_view name, Fn && body)
{
    using body_type = std::decay_t<Fn>;

    auto next = current_breadcrumb();
    next.push_back(name);
    co_await nxtrt::with_env<breadcrumb_key>(
        std::move(next), body_type{std::forward<Fn>(body)});
}

struct openai_response_stream_client
{
    using event_type = nxtrt::http::server_sent_event;

    nxtrt::value_source<event_type> & events;
    breadcrumb_output & output;

    nxtrt::task<void> stream_response()
    {
        co_await with_breadcrumb_scope(
            openai_response_scope::response,
            [this] { return stream_response_body(); });
    }

private:
    nxtrt::task<void> stream_response_body()
    {
        co_await write_expected_event(
            openai_response_event::response_created);
        co_await write_expected_event(
            openai_response_event::response_in_progress);
        co_await stream_output_item();
        co_await write_expected_event(
            openai_response_event::response_completed);
    }

    nxtrt::task<event_type> expect_event(std::string_view type)
    {
        auto * event = co_await events.peek_one();
        if (event->type != type)
            throw openai_unexpected_event{
                std::string{type},
                event->type,
            };
        co_return co_await events.take_one();
    }

    nxtrt::task<void> write_expected_event(std::string_view type)
    {
        co_await output.write_event(co_await expect_event(type));
    }

    nxtrt::task<void> stream_output_item()
    {
        co_await with_breadcrumb_scope(
            openai_response_scope::output_item,
            [this] { return stream_output_item_body(); });
    }

    nxtrt::task<void> stream_output_item_body()
    {
        co_await write_expected_event(
            openai_response_event::output_item_added);
        co_await stream_content_part();
        co_await write_expected_event(
            openai_response_event::output_item_done);
    }

    nxtrt::task<void> stream_content_part()
    {
        co_await with_breadcrumb_scope(
            openai_response_scope::content_part,
            [this] { return stream_content_part_body(); });
    }

    nxtrt::task<void> stream_content_part_body()
    {
        co_await write_expected_event(
            openai_response_event::content_part_added);

        co_await stream_output_text();

        co_await write_expected_event(
            openai_response_event::content_part_done);
    }

    nxtrt::task<void> stream_output_text()
    {
        co_await with_breadcrumb_scope(
            openai_response_scope::output_text,
            [this] { return stream_output_text_body(); });
    }

    nxtrt::task<void> stream_output_text_body()
    {
        while ((co_await events.peek_one())->type
               == openai_response_event::output_text_delta) {
            auto event = co_await events.take_one();
            auto text =
                nxtai::tools::json_string_member(event.data, "delta")
                    .value_or(std::string{});
            co_await output.write_text_delta(std::move(text));
        }

        co_await write_expected_event(
            openai_response_event::output_text_done);
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

        auto body = nxtrt::http::response_body_decoding_reader{tls, head};
        auto events = nxtrt::http::sse_event_parser(body);

        auto transcript = breadcrumb_output{output};
        auto client = openai_response_stream_client{events, transcript};

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

    auto request =
        make_request(options, co_await read_env_string("OPENAI_API_KEY"));

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
