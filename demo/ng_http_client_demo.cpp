#include "nxt/rt/task.hpp"
#include <nxt/crypto.hpp>
#include <nxt/rt/buffers.hpp>
#include <nxt/rt/cares.hpp>
#include <nxt/rt/http.hpp>
#include <nxt/unique-fd.hpp>

#if defined(__linux__)
#include <nxt/rt/uring_wand.hpp>
#else
#include <nxt/rt/kqueue_wand.hpp>
#endif

#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void set_close_on_exec(int fd)
{
    auto flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

nxt::rt::task<nxt::unique_fd> connect_address(
    nxt::rt::resolved_address address)
{
    auto fd = nxt::unique_fd{::socket(
        address.family,
        address.socktype == 0 ? SOCK_STREAM : address.socktype,
        address.protocol)};
    if (fd.get() < 0)
        throw nxt::rt::runtime_error{
            "socket: " + std::string{std::generic_category().message(errno)}};

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

using bytes = std::vector<std::byte>;

void put_u8(bytes & out, std::uint8_t value)
{
    out.push_back(static_cast<std::byte>(value));
}

void put_u16(bytes & out, std::uint16_t value)
{
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value));
}

void put_u24(bytes & out, std::uint32_t value)
{
    put_u8(out, static_cast<std::uint8_t>(value >> 16));
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value));
}

void put_bytes(bytes & out, std::span<const std::byte> input)
{
    out.insert(out.end(), input.begin(), input.end());
}

void put_text(bytes & out, std::string_view text)
{
    auto input = std::as_bytes(std::span{text});
    put_bytes(out, input);
}

void put_extension(bytes & out, std::uint16_t type, bytes body)
{
    put_u16(out, type);
    put_u16(out, static_cast<std::uint16_t>(body.size()));
    put_bytes(out, body);
}

bytes make_tls13_client_hello(std::string_view host)
{
    auto x25519 = nxt::crypto::x25519_keygen();
    auto random = nxt::crypto::random(32);
    auto session_id = nxt::crypto::random(32);

    auto extensions = bytes{};
    {
        auto names = bytes{};
        put_u8(names, 0); // host_name
        put_u16(names, static_cast<std::uint16_t>(host.size()));
        put_text(names, host);

        auto body = bytes{};
        put_u16(body, static_cast<std::uint16_t>(names.size()));
        put_bytes(body, names);
        put_extension(extensions, 0, std::move(body)); // server_name
    }
    {
        auto body = bytes{};
        put_u8(body, 2);
        put_u16(body, 0x0304); // TLS 1.3
        put_extension(extensions, 43, std::move(body)); // supported_versions
    }
    {
        auto body = bytes{};
        put_u16(body, 2);
        put_u16(body, 0x001d); // x25519
        put_extension(extensions, 10, std::move(body)); // supported_groups
    }
    {
        auto body = bytes{};
        put_u16(body, 8);
        put_u16(body, 0x0403); // ecdsa_secp256r1_sha256
        put_u16(body, 0x0804); // rsa_pss_rsae_sha256
        put_u16(body, 0x0805); // rsa_pss_rsae_sha384
        put_u16(body, 0x0401); // rsa_pkcs1_sha256
        put_extension(extensions, 13, std::move(body)); // signature_algorithms
    }
    {
        auto share = bytes{};
        put_u16(share, 0x001d); // x25519
        put_u16(share, static_cast<std::uint16_t>(x25519.public_key.size()));
        put_bytes(share, x25519.public_key);

        auto body = bytes{};
        put_u16(body, static_cast<std::uint16_t>(share.size()));
        put_bytes(body, share);
        put_extension(extensions, 51, std::move(body)); // key_share
    }
    {
        auto body = bytes{};
        put_u16(body, 9);
        put_u8(body, 8);
        put_text(body, "http/1.1");
        put_extension(extensions, 16, std::move(body)); // application_layer_protocol_negotiation
    }

    auto hello = bytes{};
    put_u16(hello, 0x0303); // ClientHello.legacy_version
    put_bytes(hello, random);
    put_u8(hello, static_cast<std::uint8_t>(session_id.size()));
    put_bytes(hello, session_id);

    put_u16(hello, 2);
    put_u16(hello, 0x1301); // TLS_AES_128_GCM_SHA256

    put_u8(hello, 1);
    put_u8(hello, 0); // legacy null compression

    put_u16(hello, static_cast<std::uint16_t>(extensions.size()));
    put_bytes(hello, extensions);

    auto handshake = bytes{};
    put_u8(handshake, 1); // client_hello
    put_u24(handshake, static_cast<std::uint32_t>(hello.size()));
    put_bytes(handshake, hello);

    auto record = bytes{};
    put_u8(record, 22); // handshake
    put_u16(record, 0x0301); // legacy record version for ClientHello
    put_u16(record, static_cast<std::uint16_t>(handshake.size()));
    put_bytes(record, handshake);
    return record;
}

void dump_hex(std::span<const std::byte> bytes)
{
    auto flags = std::cerr.flags();
    auto fill = std::cerr.fill();
    for (auto offset = std::size_t{0}; offset < bytes.size(); offset += 16) {
        std::cerr << std::setw(4) << std::setfill('0') << std::hex << offset
                  << "  ";
        auto line = bytes.subspan(offset, std::min<std::size_t>(
                                              16, bytes.size() - offset));
        for (auto byte : line)
            std::cerr << std::setw(2) << static_cast<unsigned>(
                             std::to_integer<unsigned char>(byte))
                      << ' ';
        std::cerr << '\n';
    }
    std::cerr.flags(flags);
    std::cerr.fill(fill);
}

std::uint16_t get_u16(std::span<const std::byte> input)
{
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input[0]) << 8)
        | std::to_integer<std::uint16_t>(input[1]));
}

std::uint32_t get_u24(std::span<const std::byte> input)
{
    return (std::to_integer<std::uint32_t>(input[0]) << 16)
        | (std::to_integer<std::uint32_t>(input[1]) << 8)
        | std::to_integer<std::uint32_t>(input[2]);
}

std::string_view tls_record_type_name(std::uint8_t type)
{
    switch (type) {
    case 20:
        return "change_cipher_spec";
    case 21:
        return "alert";
    case 22:
        return "handshake";
    case 23:
        return "application_data";
    default:
        return "unknown";
    }
}

std::string_view tls_handshake_type_name(std::uint8_t type)
{
    switch (type) {
    case 1:
        return "client_hello";
    case 2:
        return "server_hello";
    case 8:
        return "encrypted_extensions";
    case 11:
        return "certificate";
    case 15:
        return "certificate_verify";
    case 20:
        return "finished";
    default:
        return "unknown";
    }
}

void describe_tls_records(std::span<const std::byte> bytes)
{
    auto offset = std::size_t{0};
    auto record_index = std::size_t{0};
    while (offset + 5 <= bytes.size()) {
        auto header = bytes.subspan(offset, 5);
        auto type = std::to_integer<std::uint8_t>(header[0]);
        auto version = get_u16(header.subspan(1, 2));
        auto length = get_u16(header.subspan(3, 2));
        auto payload_offset = offset + 5;

        std::cerr << "record " << record_index << ": "
                  << tls_record_type_name(type) << " type=" << unsigned{type}
                  << " version=0x" << std::hex << version << std::dec
                  << " length=" << length << '\n';

        if (payload_offset + length > bytes.size()) {
            std::cerr << "  partial record body: have "
                      << (bytes.size() - payload_offset) << " bytes\n";
            return;
        }

        if (type == 22 && length >= 4) {
            auto payload = bytes.subspan(payload_offset, length);
            auto handshake_type = std::to_integer<std::uint8_t>(payload[0]);
            auto handshake_length = get_u24(payload.subspan(1, 3));
            std::cerr << "  handshake: "
                      << tls_handshake_type_name(handshake_type)
                      << " type=" << unsigned{handshake_type}
                      << " length=" << handshake_length << '\n';
        }

        offset = payload_offset + length;
        ++record_index;
    }

    if (offset != bytes.size())
        std::cerr << "trailing partial record header: " << (bytes.size() - offset)
                  << " bytes\n";
}

nxt::rt::task<void> probe_tls13(nxt::rt::http::url url)
{
    auto socket = co_await connect_tcp(url.host, url.port);
    auto hello = make_tls13_client_hello(url.host);

    std::cerr << "sending TLS 1.3 ClientHello (" << hello.size() << " bytes)\n";
    auto socket_output = nxt::rt::byte_writer<nxt::rt::socket_sink>{
        nxt::rt::socket_sink{socket.get()},
        4096,
    };
    co_await socket_output.write_all(hello);

    auto source = nxt::rt::socket_source{socket.get()};
    auto input_storage = std::vector<std::byte>(4096);
    auto reader = nxt::rt::byte_reader<nxt::rt::socket_source>{
        source,
        std::span{input_storage},
    };

    auto answer = co_await reader.take_some();
    if (!answer) {
        std::cerr << "server closed without sending a TLS record\n";
        co_return;
    }

    std::cerr << "received " << answer->size() << " bytes\n";
    describe_tls_records(*answer);
    dump_hex(*answer);
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
                {"User-Agent", "nxt-ng-http-demo/0"},
                {"Accept", "*/*"},
            },
        .body = {},
    };

    auto socket_output = nxt::rt::byte_writer<nxt::rt::socket_sink>{
        nxt::rt::socket_sink{socket.get()},
        4096,
    };
    co_await socket_output.write_all(nxt::rt::http::serialize(request));

    auto source = nxt::rt::socket_source{socket.get()};
    auto input_storage = std::vector<std::byte>(64 * 1024);
    auto reader = nxt::rt::byte_reader<nxt::rt::socket_source>{
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
    nxt::rt::run([url = std::move(url)]() mutable {
        return fetch(std::move(url));
    });
#elif NXT_RT_HAS_KQUEUE
    nxt::rt::run_with_kqueue([url = std::move(url)]() mutable {
        return fetch(std::move(url));
    });
#else
    static_assert(NXT_RT_HAS_KQUEUE, "ng http demo needs a runtime wand");
#endif

    return 0;
} catch (std::exception const & error) {
    std::cerr << "ng-http-client-demo: " << error.what() << '\n';
    return 1;
}
