#include "nxt/rt/task.hpp"
#include <nxt/http.hpp>
#include <nxt/rt/buffers.hpp>
#include <nxt/rt/cares.hpp>
#include <nxt/rt/http.hpp>
#include <nxt/tls.hpp>
#include <nxt/tls/cert.hpp>
#include <nxt/unique-fd.hpp>
#include <nxt/llm/responses_request.hpp>

#if defined(__linux__)
#  include <nxt/rt/uring_wand.hpp>
#else
#  include <nxt/rt/kqueue_wand.hpp>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using bytes = nxt::tls::bytes;

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

std::string make_https_request(nxt::rt::http::url const & url)
{
    if (url.host == "api.openai.com") {
        auto * api_key = std::getenv("OPENAI_API_KEY");
        nxt::tls::require_tls(
            api_key != nullptr && std::string_view{api_key}.size() > 0,
            "OPENAI_API_KEY is not set");
        auto request = nxt::llm::responses::openai_responses_request{
            .api_key = api_key,
            .model = "gpt-5-mini",
            .input =
                "Reply in one short sentence from a handmade TLS 1.3 client.",
            .max_output_tokens = 64,
            .reasoning_effort = "minimal",
            .reasoning_summary = "",
            .store = false,
        };
        auto http_request =
            nxt::llm::responses::openai_responses_http_request(request);
        for (auto & header : http_request.headers) {
            if (header.name == "Connection")
                header.value = "close";
        }
        return nxt::http::serialize(http_request);
    }

    auto request = nxt::rt::http::request{
        .method = "GET",
        .target = url.target,
        .host = nxt::rt::http::host_header(url),
        .headers =
            {
                {"User-Agent", "nxt-tls-demo/0"},
                {"Accept", "*/*"},
            },
        .body = {},
    };
    return nxt::rt::http::serialize(request);
}

struct http_response_progress
{
    std::string bytes;
    std::optional<std::size_t> body_offset;
    std::optional<std::size_t> content_length;
    bool chunked = false;

    bool append(std::span<const std::byte> chunk)
    {
        bytes.append(
            reinterpret_cast<const char *>(chunk.data()), chunk.size());

        if (!body_offset) {
            auto head_end = bytes.find("\r\n\r\n");
            if (head_end == std::string::npos)
                return false;

            auto head_text = std::string_view{bytes}.substr(0, head_end);
            auto head = nxt::rt::http::parse_response_head(
                std::as_bytes(std::span{head_text}));
            body_offset = head_end + 4;
            content_length = nxt::rt::http::content_length(head);
            chunked = nxt::rt::http::is_chunked(head);
        }

        if (chunked)
            return bytes.find("\r\n0\r\n\r\n", *body_offset)
                   != std::string::npos;

        if (content_length)
            return bytes.size() - *body_offset >= *content_length;

        return false;
    }
};

nxt::rt::task<void> probe_tls13(nxt::rt::http::url url)
{
    auto socket = co_await connect_tcp(url.host, url.port);
    auto hello = nxt::tls::make_tls13_client_hello(url.host);

    std::cerr << "sending TLS 1.3 ClientHello (" << hello.record.size()
              << " bytes)\n";
    auto sink = nxt::rt::socket_sink{socket.get()};
    auto socket_output = nxt::rt::byte_writer{sink, 4096};
    co_await socket_output.write_all(hello.record);

    auto source = nxt::rt::socket_source{socket.get()};
    auto input_storage = std::vector<std::byte>(18 * 1024);
    auto reader = nxt::rt::byte_reader{
        source,
        std::span{input_storage},
    };

    auto record = co_await nxt::tls::read_tls_record(reader);
    nxt::tls::describe_tls_record(0, record);
    auto server_hello = nxt::tls::parse_tls13_server_hello(record);
    nxt::tls::describe_server_hello(server_hello);

    auto shared_secret = nxt::crypto::x25519_dh(
        hello.key_pair.secret_key, server_hello.key_share);
    nxt::tls::require_tls(
        shared_secret.has_value(), "X25519 shared secret failed");
    std::cerr << "computed X25519 shared secret: " << shared_secret->size()
              << " bytes\n";

    auto index = std::size_t{1};
    auto transcript =
        nxt::tls::join_bytes(hello.handshake, server_hello.handshake);
    auto handshake_keys =
        nxt::tls::derive_tls13_handshake_keys(*shared_secret, transcript);
    std::cerr << "derived handshake AES-128-GCM keys and IVs\n";

    record = co_await nxt::tls::read_tls_record(reader);
    nxt::tls::describe_tls_record(index, record);
    if (record.type == 20) {
        nxt::tls::dump_hex(record.payload);
        ++index;
        record = co_await nxt::tls::read_tls_record(reader);
        nxt::tls::describe_tls_record(index, record);
    }

    auto leaf_public_key = std::optional<bytes>{};
    auto saw_server_finished = false;
    while (true) {
        auto plaintext =
            nxt::tls::open_tls13_record(handshake_keys.server, record);
        std::cerr << "decrypted inner record: "
                  << nxt::tls::tls_record_type_name(plaintext.inner_type)
                  << " type=" << unsigned{plaintext.inner_type}
                  << " length=" << plaintext.content.size() << '\n';
        if (plaintext.inner_type == 22)
            nxt::tls::describe_handshake_messages(plaintext.content);
        if (plaintext.content.size() <= 256)
            nxt::tls::dump_hex(plaintext.content);

        if (plaintext.inner_type == 22) {
            for (auto const & message :
                 nxt::tls::split_handshake_messages(plaintext.content)) {
                auto type = std::to_integer<std::uint8_t>(message[0]);
                if (type == 11) {
                    auto cert = nxt::tls::parse_tls13_certificate(message);
                    leaf_public_key =
                        nxt::tls::extract_p256_public_key_from_certificate(
                            cert.leaf_der);
                    std::cerr << "parsed TLS certificate list: "
                              << cert.chain_der.size() << " certificates\n";
                    std::cerr << "extracted leaf P-256 public key: "
                              << leaf_public_key->size() << " bytes\n";
                } else if (type == 15) {
                    nxt::tls::require_tls(
                        leaf_public_key.has_value(),
                        "certificate_verify arrived before certificate public key");
                    auto cert_verify =
                        nxt::tls::parse_tls13_certificate_verify(message);
                    auto ok = nxt::tls::verify_certificate_verify(
                        *leaf_public_key, transcript, cert_verify);
                    nxt::tls::require_tls(
                        ok, "CertificateVerify signature failed");
                    std::cerr << "verified CertificateVerify signature\n";
                } else if (type == 20) {
                    auto received = nxt::tls::parse_tls13_finished(message);
                    auto ok = nxt::tls::verify_finished(
                        handshake_keys.server.traffic_secret,
                        transcript,
                        received);
                    nxt::tls::require_tls(
                        ok, "server Finished verification failed");
                    std::cerr << "verified server Finished\n";
                    saw_server_finished = true;
                }
                nxt::tls::put_bytes(transcript, message);
            }
        }

        if (saw_server_finished)
            break;
        nxt::tls::require_tls(
            reader.buffered_size() > 0,
            "server flight ended before Finished");
        ++index;
        record = co_await nxt::tls::read_tls_record(reader);
        nxt::tls::describe_tls_record(index, record);
    }

    auto application_keys = nxt::tls::derive_tls13_application_keys(
        handshake_keys.secret, transcript);
    std::cerr << "derived application AES-128-GCM keys and IVs\n";

    auto client_finished = nxt::tls::make_finished_message(
        handshake_keys.client.traffic_secret, transcript);
    auto encrypted_finished = nxt::tls::seal_tls13_record(
        handshake_keys.client, 22, client_finished);
    co_await socket_output.write_all(encrypted_finished);
    nxt::tls::put_bytes(transcript, client_finished);
    std::cerr << "sent encrypted client Finished\n";

    auto request_text = make_https_request(url);
    auto encrypted_request = nxt::tls::seal_tls13_record(
        application_keys.client, 23, nxt::rt::as_bytes(request_text));
    co_await socket_output.write_all(encrypted_request);
    std::cerr << "sent encrypted HTTP request\n";

    auto http_response = http_response_progress{};
    while (true) {
        record = co_await nxt::tls::read_tls_record(reader);
        nxt::tls::describe_tls_record(++index, record);
        auto plaintext =
            nxt::tls::open_tls13_record(application_keys.server, record);
        std::cerr << "decrypted application record: "
                  << nxt::tls::tls_record_type_name(plaintext.inner_type)
                  << " type=" << unsigned{plaintext.inner_type}
                  << " length=" << plaintext.content.size() << '\n';
        if (plaintext.inner_type == 23) {
            auto stdout_sink = nxt::rt::standard_output();
            co_await nxt::rt::write_all(stdout_sink, plaintext.content);
            if (http_response.append(plaintext.content)) {
                std::cerr << "read complete HTTP response body\n";
                break;
            }
        } else if (plaintext.inner_type == 21) {
            std::cerr << "received encrypted TLS alert\n";
            nxt::tls::dump_hex(plaintext.content);
            break;
        }
    }
}

nxt::rt::task<void> fetch(nxt::rt::http::url url)
{
    if (url.tls)
        co_return co_await probe_tls13(std::move(url));

    auto socket = co_await connect_tcp(url.host, url.port);
    auto request = nxt::rt::http::request{
        .method = "GET",
        .target = url.target,
        .host = nxt::rt::http::host_header(url),
        .headers =
            {
                {"User-Agent", "nxt-http-demo/0"},
                {"Accept", "*/*"},
            },
        .body = {},
    };

    auto sink = nxt::rt::socket_sink{socket.get()};
    auto socket_output = nxt::rt::byte_writer{sink, 4096};
    co_await socket_output.write_all(nxt::rt::http::serialize(request));

    auto source = nxt::rt::socket_source{socket.get()};
    auto input_storage = std::vector<std::byte>(64 * 1024);
    auto reader = nxt::rt::byte_reader{
        source,
        std::span{input_storage},
    };

    auto head = co_await nxt::rt::http::read_response_head(reader);
    std::cerr << head.version << ' ' << head.status << ' ' << head.reason
              << "\n";
    for (auto const & header : head.headers)
        std::cerr << header.name << ": " << header.value << "\n";
    std::cerr << "\n";

    auto stdout_writer = nxt::rt::standard_output_writer(64 * 1024);
    auto body = nxt::rt::http::read_response_body(reader, head);
    while (auto chunk = co_await body.next())
        co_await stdout_writer.write(*chunk);
    co_await stdout_writer.flush();
}

} // namespace

int main(int argc, char ** argv)
try {
    auto url = nxt::rt::http::parse_url(
        argc > 1 ? std::string_view{argv[1]}
                 : std::string_view{"https://less.rest/"});

#if defined(__linux__)
    nxt::rt::run(
        [url = std::move(url)]() mutable { return fetch(std::move(url)); });
#elif NXT_RT_HAS_KQUEUE
    nxt::rt::run_with_kqueue(
        [url = std::move(url)]() mutable { return fetch(std::move(url)); });
#else
    static_assert(NXT_RT_HAS_KQUEUE, "http demo needs a runtime wand");
#endif

    return 0;
} catch (std::exception const & error) {
    std::cerr << "nxt-http-client-demo: " << error.what() << '\n';
    return 1;
}
