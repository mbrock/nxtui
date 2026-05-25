#include <nxt/rt.hpp>
#include <nxt/rt/cares.hpp>
#include <nxt/http.hpp>
#include <nxt/tls.hpp>
#include <nxt/tls/cert.hpp>
#include <nxt/unique-fd.hpp>
#include <nxtai/openai_types.hpp>
#include <nxtai/responses_request.hpp>

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using llm_request = nxt::ai::responses::openai_responses_request;
using bytes = nxt::tls::bytes;

constexpr std::size_t default_max_output_tokens = 128000;

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

void set_close_on_exec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

nxt::rt::task<nxt::unique_fd>
connect_address(nxt::rt::resolved_address address)
{
    auto fd = nxt::unique_fd{::socket(
        address.family,
        address.socktype == 0 ? SOCK_STREAM : address.socktype,
        address.protocol)};
    if (fd.get() < 0)
        throw nxt::rt::runtime_error{
            "socket: "
            + std::string{std::generic_category().message(errno)}};

    set_close_on_exec(fd.get());
    co_await nxt::rt::op::connect::from(
        fd.get(), address.sockaddr_ptr(), address.address_size);
    co_return std::move(fd);
}

nxt::rt::task<nxt::unique_fd>
connect_tcp(std::string host, std::string port)
{
    auto resolver = nxt::rt::cares_resolver{};
    auto addresses = co_await resolver.getaddrinfo(host, port);
    if (addresses.empty())
        throw nxt::rt::runtime_error{"no addresses resolved for " + host};

    co_return co_await nxt::rt::wait_any_range(
        addresses | std::views::transform([](auto const & address) {
            return connect_address(address);
        }));
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

class sse_http_parser
{
public:
    [[nodiscard]] std::vector<stream_event>
    append(std::span<const std::byte> chunk)
    {
        bytes_.append(
            reinterpret_cast<const char *>(chunk.data()), chunk.size());

        auto events = std::vector<stream_event>{};
        if (!head_) {
            auto head_end = bytes_.find("\r\n\r\n");
            if (head_end == std::string::npos)
                return events;

            auto head_text = std::string_view{bytes_}.substr(0, head_end);
            head_ = nxt::rt::http::parse_response_head(
                std::as_bytes(std::span{head_text}));
            if (!response_status_is_success(*head_))
                throw nxt::rt::runtime_error{
                    "OpenAI Responses HTTP error: "
                    + std::to_string(head_->status) + " " + head_->reason};
            if (!response_content_type_is(*head_, "text/event-stream"))
                throw nxt::rt::runtime_error{
                    "OpenAI Responses expected text/event-stream"};

            chunked_ = nxt::rt::http::is_chunked(*head_);
            content_length_ = nxt::rt::http::content_length(*head_);
            body_pos_ = head_end + 4;
            body_start_ = body_pos_;
        }

        if (done_)
            return events;

        if (chunked_)
            decode_chunked(events);
        else
            decode_raw_body(events);

        return events;
    }

    [[nodiscard]] bool done() const noexcept
    {
        return done_;
    }

private:
    void feed_sse(std::string_view body, std::vector<stream_event> & events)
    {
        for (auto & event : sse_.feed(body)) {
            if (event.data == "[DONE]") {
                done_ = true;
                continue;
            }
            auto terminal = event.type == "response.completed"
                            || event.type == "response.incomplete"
                            || event.type == "response.failed";
            events.push_back(
                stream_event{
                    .type = std::move(event.type),
                    .data = std::move(event.data),
                });
            if (terminal)
                done_ = true;
        }
    }

    void decode_chunked(std::vector<stream_event> & events)
    {
        while (!done_) {
            auto line_end = bytes_.find("\r\n", body_pos_);
            if (line_end == std::string::npos)
                return;

            auto size = nxt::rt::http::parse_chunk_size(
                std::as_bytes(
                    std::span{
                        std::string_view{bytes_}.substr(
                            body_pos_, line_end - body_pos_)}));
            auto data_begin = line_end + 2;
            auto data_end = data_begin + size;
            if (bytes_.size() < data_end + 2)
                return;
            if (bytes_.compare(data_end, 2, "\r\n") != 0)
                throw nxt::rt::runtime_error{
                    "chunk data was not followed by CRLF"};

            if (size == 0) {
                done_ = true;
                return;
            }

            feed_sse(
                std::string_view{bytes_}.substr(data_begin, size), events);
            body_pos_ = data_end + 2;
            compact();
        }
    }

    void decode_raw_body(std::vector<stream_event> & events)
    {
        if (body_pos_ >= bytes_.size())
            return;

        auto available = bytes_.size() - body_pos_;
        if (content_length_) {
            auto consumed = body_pos_ - body_start_;
            available = std::min(available, *content_length_ - consumed);
        }

        feed_sse(std::string_view{bytes_}.substr(body_pos_, available), events);
        body_pos_ += available;
        if (content_length_ && body_pos_ - body_start_ >= *content_length_)
            done_ = true;
        compact();
    }

    void compact()
    {
        if (body_pos_ < 32 * 1024)
            return;
        bytes_.erase(0, body_pos_);
        body_start_ = body_start_ > body_pos_ ? body_start_ - body_pos_ : 0;
        body_pos_ = 0;
    }

    std::string bytes_;
    std::optional<nxt::rt::http::response_head> head_;
    nxt::http::server_sent_event_parser sse_;
    std::size_t body_pos_ = 0;
    std::size_t body_start_ = 0;
    std::optional<std::size_t> content_length_;
    bool chunked_ = false;
    bool done_ = false;
};

nxt::rt::task<void> stream_openai_response(const llm_request & request)
{
    auto socket = co_await connect_tcp("api.openai.com", "443");
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

    auto hello = nxt::tls::make_tls13_client_hello("api.openai.com");
    co_await socket_output.write_all(hello.record);

    auto record = co_await nxt::tls::read_tls_record(reader);
    auto server_hello = nxt::tls::parse_tls13_server_hello(record);
    auto shared_secret = nxt::crypto::x25519_dh(
        hello.key_pair.secret_key, server_hello.key_share);
    nxt::tls::require_tls(
        shared_secret.has_value(), "X25519 shared secret failed");

    auto transcript =
        nxt::tls::join_bytes(hello.handshake, server_hello.handshake);
    auto handshake_keys =
        nxt::tls::derive_tls13_handshake_keys(*shared_secret, transcript);

    auto leaf_public_key = std::optional<bytes>{};
    auto saw_server_finished = false;
    while (!saw_server_finished) {
        record = co_await nxt::tls::read_tls_record(reader);
        if (record.type == 20)
            continue;

        auto plaintext =
            nxt::tls::open_tls13_record(handshake_keys.server, record);
        if (plaintext.inner_type != 22)
            continue;

        for (auto const & message :
             nxt::tls::split_handshake_messages(plaintext.content)) {
            auto type = std::to_integer<std::uint8_t>(message[0]);
            if (type == 11) {
                auto cert = nxt::tls::parse_tls13_certificate(message);
                leaf_public_key =
                    nxt::tls::extract_p256_public_key_from_certificate(
                        cert.leaf_der);
            } else if (type == 15) {
                nxt::tls::require_tls(
                    leaf_public_key.has_value(),
                    "certificate_verify arrived before certificate");
                auto cert_verify =
                    nxt::tls::parse_tls13_certificate_verify(message);
                auto ok = nxt::tls::verify_certificate_verify(
                    *leaf_public_key, transcript, cert_verify);
                nxt::tls::require_tls(
                    ok, "CertificateVerify signature failed");
            } else if (type == 20) {
                auto received = nxt::tls::parse_tls13_finished(message);
                auto ok = nxt::tls::verify_finished(
                    handshake_keys.server.traffic_secret,
                    transcript,
                    received);
                nxt::tls::require_tls(ok, "server Finished failed");
                saw_server_finished = true;
            }
            nxt::tls::put_bytes(transcript, message);
        }
    }

    auto application_keys = nxt::tls::derive_tls13_application_keys(
        handshake_keys.secret, transcript);
    auto client_finished = nxt::tls::make_finished_message(
        handshake_keys.client.traffic_secret, transcript);
    co_await socket_output.write_all(
        nxt::tls::seal_tls13_record(handshake_keys.client, 22, client_finished));
    nxt::tls::put_bytes(transcript, client_finished);

    auto http_request =
        nxt::ai::responses::openai_responses_http_request(request);
    for (auto & header : http_request.headers) {
        if (header.name == "Connection")
            header.value = "close";
    }
    auto request_text = nxt::http::serialize(http_request);
    co_await socket_output.write_all(
        nxt::tls::seal_tls13_record(
            application_keys.client,
            23,
            nxt::rt::as_bytes(request_text)));

    auto parser = sse_http_parser{};
    while (!parser.done()) {
        record = co_await nxt::tls::read_tls_record(reader);
        auto plaintext =
            nxt::tls::open_tls13_record(application_keys.server, record);
        if (plaintext.inner_type == 21)
            throw nxt::rt::runtime_error{"received TLS alert"};
        if (plaintext.inner_type != 23)
            continue;

        for (auto const & event : parser.append(plaintext.content)) {
            if (event.type == "response.output_text.delta") {
                auto delta =
                    event.read<nxt::ai::openai::text_delta_event>();
                std::cout << delta.delta << std::flush;
            } else if (event.type == "response.failed"
                       || event.type == "response.incomplete") {
                throw nxt::rt::runtime_error{
                    "OpenAI Responses terminal event: " + event.type};
            }
        }
    }

    std::cout << '\n';
}

nxt::rt::task<int> run_nxtllm(cli_options options)
{
    auto request = make_request(options);

    if (!options.oneshot_prompt) {
        std::cout
            << "nxtllm is now on nxt::rt; the interactive HUD is still being "
               "ported.\n"
            << "Pass a prompt for one-shot request construction, or use "
               "--dump-request to inspect the JSON envelope.\n";
        co_return EXIT_SUCCESS;
    }

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

    co_await stream_openai_response(request);
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
