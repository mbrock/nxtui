#include <nxtrt/app.hpp>
#include <nxtrt/buffers.hpp>
#include <nxtrt/bell.hpp>
#include <nxtrt/net.hpp>
#include <nxtrt/pool.hpp>
#include <nxt/unique-fd.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct bench_options
{
    std::size_t clients = 128;
    std::size_t messages = 256;
    std::size_t payload_size = 64;
    std::chrono::milliseconds timeout = 30s;
};

struct bench_stats
{
    std::size_t accepted = 0;
    std::size_t echoed_bytes = 0;
    std::size_t client_sent = 0;
    std::size_t client_received = 0;
};

nxtrt::task<std::size_t> send_all_socket(
    int fd,
    std::span<const std::byte> bytes)
{
    auto remaining = bytes;
    while (!remaining.empty()) {
        auto written = std::size_t{0};
        try {
            written = co_await nxtrt::op::send_some{fd, remaining, 0};
        } catch (const nxtrt::interrupted_system_call &) {
            continue;
        }
        if (written == 0)
            throw std::runtime_error{"socket send made no progress"};
        if (written > remaining.size())
            throw std::runtime_error{"socket send overreported bytes"};
        remaining = remaining.subspan(written);
    }
    co_return bytes.size();
}

nxtrt::task<void> recv_exact_socket(
    int fd,
    std::span<std::byte> bytes)
{
    auto remaining = bytes;
    while (!remaining.empty()) {
        auto read = std::size_t{0};
        try {
            read = co_await nxtrt::op::recv_some{fd, remaining, 0};
        } catch (const nxtrt::interrupted_system_call &) {
            continue;
        }
        if (read == 0)
            throw std::runtime_error{"socket closed before echo completed"};
        if (read > remaining.size())
            throw std::runtime_error{"socket recv overreported bytes"};
        remaining = remaining.subspan(read);
    }
}

nxtrt::task<void> echo_connection(
    nxt::unique_fd fd,
    std::size_t payload_size,
    bench_stats * stats)
{
    auto buffer = std::vector<std::byte>(payload_size);

    while (true) {
        auto read = std::size_t{0};
        try {
            read = co_await nxtrt::op::recv_some{fd.get(), std::span{buffer}};
        } catch (const nxtrt::interrupted_system_call &) {
            continue;
        }

        if (read == 0)
            co_return;

        co_await send_all_socket(
            fd.get(),
            std::span<const std::byte>{buffer}.first(read));
        stats->echoed_bytes += read;
    }
}

struct echo_idea
{
    nxt::unique_fd fd;
    std::size_t payload_size;
    bench_stats * stats;

    nxtrt::task<void> operator()() &
    {
        return echo_connection(std::move(fd), payload_size, stats);
    }
};

/// Accepts `clients` connections, then ends.
class accepted_echoes final : public nxtrt::feed<echo_idea>
{
public:
    accepted_echoes(
        int listener,
        std::size_t clients,
        std::size_t payload_size,
        bench_stats * stats)
        : feed(1)
        , listener_(listener)
        , clients_(clients)
        , payload_size_(payload_size)
        , stats_(stats)
    {}

private:
    nxtrt::task<std::optional<echo_idea>> next_value() override
    {
        if (accepted_ == clients_)
            co_return std::nullopt;
        auto fd = co_await nxtrt::net::accept(listener_);
        ++accepted_;
        stats_->accepted += 1;
        co_return echo_idea{std::move(fd), payload_size_, stats_};
    }

    int listener_;
    std::size_t clients_;
    std::size_t payload_size_;
    bench_stats * stats_;
    std::size_t accepted_ = 0;
};

nxtrt::task<void> echo_server(
    int listener,
    std::size_t clients,
    std::size_t payload_size,
    bench_stats * stats)
{
    auto accepted = accepted_echoes{listener, clients, payload_size, stats};
    co_await nxtrt::drain(accepted, clients);
}

nxtrt::task<void> echo_client(
    sockaddr_in address,
    std::size_t messages,
    std::span<const std::byte> payload,
    bench_stats * stats)
{
    auto fd = co_await nxtrt::net::connect(address);

    auto echoed = std::vector<std::byte>(payload.size());
    for (auto i = std::size_t{0}; i < messages; ++i) {
        auto sent = co_await send_all_socket(fd.get(), payload);
        stats->client_sent += sent;

        co_await recv_exact_socket(fd.get(), std::span{echoed});
        if (!std::ranges::equal(payload, echoed))
            throw std::runtime_error{"echo payload mismatch"};
        stats->client_received += echoed.size();
    }
}

nxtrt::task<void> run_clients(
    bench_options options,
    sockaddr_in address,
    std::span<const std::byte> payload,
    bench_stats * stats)
{
    auto clients = std::vector<nxtrt::task<void>>{};
    clients.reserve(options.clients);
    for (auto i = std::size_t{0}; i < options.clients; ++i)
        clients.push_back(
            echo_client(address, options.messages, payload, stats));
    auto outcomes = co_await nxtrt::settle_range(
        std::move(clients), nxtrt::fail_fast_group{});
    for (auto & outcome : outcomes)
        if (!outcome)
            nxtrt::rethrow(outcome.error());
}

struct echo_load_group : nxtrt::primary_group
{
    bool should_stop(std::size_t index, bool failed) const noexcept override
    {
        return primary_group::should_stop(index, failed) || failed;
    }
};

nxtrt::task<void> run_echo_load(
    bench_options options,
    sockaddr_in address,
    int listener,
    std::span<const std::byte> payload,
    bench_stats * stats)
{
    // The clients are the primary job: when they finish, or when anything
    // fails (including the timeout), the rest are stopped.
    auto [clients, server, deadline] = co_await nxtrt::settle(
        std::tuple{
            run_clients(options, address, payload, stats),
            echo_server(
                listener, options.clients, options.payload_size, stats),
            nxtrt::timeout_after(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    options.timeout)),
        },
        echo_load_group{});

    if (!deadline && !nxtrt::is_operation_cancelled(deadline.error()))
        nxtrt::rethrow(deadline.error());
    if (!clients)
        nxtrt::rethrow(clients.error());
    if (!server && !nxtrt::is_operation_cancelled(server.error()))
        nxtrt::rethrow(server.error());
}

std::size_t parse_size(std::string_view text, std::string_view name)
{
    auto pos = std::size_t{0};
    auto value = std::stoull(std::string{text}, &pos);
    if (pos != text.size())
        throw std::runtime_error{"invalid numeric value for " + std::string{name}};
    if (value > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error{"numeric value too large for " + std::string{name}};
    return static_cast<std::size_t>(value);
}

std::string_view option_value(
    int & i,
    int argc,
    char ** argv,
    std::string_view arg,
    std::string_view name)
{
    auto prefix = std::string{name} + "=";
    if (arg.starts_with(prefix))
        return arg.substr(prefix.size());
    if (arg == name) {
        if (i + 1 == argc)
            throw std::runtime_error{"missing value for " + std::string{name}};
        ++i;
        return argv[i];
    }
    return {};
}

void print_usage(char const * program)
{
    std::cout
        << "usage: " << program << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --clients N       concurrent loopback clients (default 128)\n"
        << "  --messages N      messages per client (default 256)\n"
        << "  --payload N       payload bytes per message (default 64)\n"
        << "  --timeout-ms N    maximum benchmark runtime (default 30000)\n";
}

bench_options parse_options(int argc, char ** argv)
{
    auto options = bench_options{};
    for (auto i = 1; i < argc; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }

        if (auto value = option_value(i, argc, argv, arg, "--clients");
            !value.empty()) {
            options.clients = parse_size(value, "--clients");
        } else if (auto value = option_value(i, argc, argv, arg, "--messages");
                   !value.empty()) {
            options.messages = parse_size(value, "--messages");
        } else if (auto value = option_value(i, argc, argv, arg, "--payload");
                   !value.empty()) {
            options.payload_size = parse_size(value, "--payload");
        } else if (auto value = option_value(i, argc, argv, arg, "--timeout-ms");
                   !value.empty()) {
            options.timeout =
                std::chrono::milliseconds{parse_size(value, "--timeout-ms")};
        } else {
            throw std::runtime_error{"unknown option " + std::string{arg}};
        }
    }

    if (options.clients == 0)
        throw std::runtime_error{"--clients must be greater than zero"};
    if (options.messages == 0)
        throw std::runtime_error{"--messages must be greater than zero"};
    if (options.payload_size == 0)
        throw std::runtime_error{"--payload must be greater than zero"};
    if (options.timeout.count() == 0)
        throw std::runtime_error{"--timeout-ms must be greater than zero"};

    return options;
}

std::vector<std::byte> make_payload(std::size_t size)
{
    auto payload = std::vector<std::byte>(size);
    for (auto i = std::size_t{0}; i < payload.size(); ++i)
        payload[i] = static_cast<std::byte>((i * 131 + 17) & 0xff);
    return payload;
}

void print_result(
    bench_options const & options,
    bench_stats const & stats,
    std::chrono::nanoseconds elapsed)
{
    auto seconds =
        std::chrono::duration<double>{elapsed}.count();
    auto round_trips = options.clients * options.messages;
    auto payload_bytes = round_trips * options.payload_size;
    auto total_client_bytes =
        stats.client_sent + stats.client_received;
    auto mib = static_cast<double>(total_client_bytes) / (1024.0 * 1024.0);

    std::cout
        << "{\n"
        << "  \"benchmark\": \"tcp_echo\",\n"
        << "  \"clients\": " << options.clients << ",\n"
        << "  \"messages_per_client\": " << options.messages << ",\n"
        << "  \"payload_bytes\": " << options.payload_size << ",\n"
        << "  \"round_trips\": " << round_trips << ",\n"
        << "  \"expected_payload_bytes_each_way\": " << payload_bytes << ",\n"
        << "  \"accepted_connections\": " << stats.accepted << ",\n"
        << "  \"client_sent_bytes\": " << stats.client_sent << ",\n"
        << "  \"client_received_bytes\": " << stats.client_received << ",\n"
        << "  \"server_echoed_bytes\": " << stats.echoed_bytes << ",\n"
        << "  \"elapsed_seconds\": " << seconds << ",\n"
        << "  \"round_trips_per_second\": "
        << (static_cast<double>(round_trips) / seconds) << ",\n"
        << "  \"client_throughput_mib_per_second\": " << (mib / seconds)
        << "\n"
        << "}\n";
}

} // namespace

int main(int argc, char ** argv)
{
    try {
        auto options = parse_options(argc, argv);
        auto listener = nxtrt::net::listen_tcp_loopback();
        auto address = nxtrt::net::socket_address(listener.get());
        auto payload = make_payload(options.payload_size);
        auto stats = bench_stats{};
        auto runtime = nxtrt::runtime{};

        auto started = std::chrono::steady_clock::now();
        runtime.run(
            run_echo_load,
            options,
            address,
            listener.get(),
            std::span<const std::byte>{payload},
            &stats);
        auto elapsed = std::chrono::steady_clock::now() - started;

        print_result(options, stats, elapsed);
        return 0;
    } catch (...) {
        std::cerr << "nxt-echo-bench:\n";
        std::cerr << nxtrt::format_exception_tree(std::current_exception())
                  << "\n";
        return 1;
    }
}
