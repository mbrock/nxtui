// Standalone Windows runner: test.hpp's watchdog uses POSIX signals.
#include <nxtrt/arch.hpp>

#include <array>
#include <cstdio>
#include <system_error>

using namespace std::chrono_literals;
using namespace nxtrt;

static_assert(std::same_as<arch::wand, iocp_wand>);
static_assert(sizeof(socket_handle) == sizeof(void *));

namespace {
void check(bool condition, const char * message)
{
    if (!condition)
        throw runtime_error{message};
}

struct socket_owner
{
    SOCKET value = WSASocketW(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

    socket_owner()
    {
        check(value != INVALID_SOCKET, "WSASocketW failed");
    }

    explicit socket_owner(SOCKET value)
        : value(value)
    {
    }

    ~socket_owner()
    {
        closesocket(value);
    }

    socket_owner(const socket_owner &) = delete;
    socket_owner & operator=(const socket_owner &) = delete;
};

struct listener
{
    socket_owner socket;
    sockaddr_in address{};

    listener()
    {
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(
            bind(
                socket.value,
                reinterpret_cast<sockaddr *>(&address),
                sizeof(address))
                == 0,
            "bind failed");
        check(listen(socket.value, SOMAXCONN) == 0, "listen failed");
        int size = sizeof(address);
        check(
            getsockname(
                socket.value, reinterpret_cast<sockaddr *>(&address), &size)
                == 0,
            "getsockname failed");
    }
};

struct connected_pair
{
    listener server;
    socket_owner client;
    socket_owner peer;

    connected_pair()
        : peer(INVALID_SOCKET)
    {
        check(
            ::connect(
                client.value,
                reinterpret_cast<sockaddr *>(&server.address),
                sizeof(server.address))
                == 0,
            "fixture connect failed");
        peer.value = ::accept(server.socket.value, nullptr, nullptr);
        check(peer.value != INVALID_SOCKET, "fixture accept failed");
    }
};

struct file_owner
{
    HANDLE value;

    file_owner()
    {
        CREATEFILE2_EXTENDED_PARAMETERS parameters{};
        parameters.dwSize = sizeof(parameters);
        parameters.dwFileFlags =
            FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE;
        value = CreateFile2(
            L"iocp-test.tmp",
            GENERIC_READ | GENERIC_WRITE,
            0,
            CREATE_ALWAYS,
            &parameters);
        check(value != INVALID_HANDLE_VALUE, "CreateFile2 failed");
    }

    ~file_owner()
    {
        CloseHandle(value);
    }
};

task<void> sleep_for(std::chrono::nanoseconds duration)
{
    co_await op::timeout::after(duration);
}

task<void> timers()
{
    co_await op::manual{};
    co_await op::timeout::after(-1ms);
    auto start = std::chrono::steady_clock::now();
    co_await op::timeout::after(3ms);
    check(
        std::chrono::steady_clock::now() - start >= 3ms,
        "timer fired early");
    co_await when_all(sleep_for(9ms), sleep_for(1ms));
}

task<void> timer_wait()
{
    co_await op::timeout::after(1h);
}

void cancel_timer()
{
    auto w = iocp_wand{};
    auto d = deck{&w};
    auto root = root_task{d, timer_wait};
    root.start();
    d.run_ready();
    root.inner().request_stop();
    w.run_until_done(d, root.inner());
    bool cancelled = false;
    try {
        std::move(root.inner()).result();
    } catch (operation_cancelled const &) {
        cancelled = true;
    }
    check(cancelled, "timer stop was not delivered");
    w.poll(d); // retire the cancelled timer; stale events must not survive
}

task<void> files(HANDLE file)
{
    auto bytes = std::array{
        std::byte{0x13}, std::byte{0x47}, std::byte{0x8a}, std::byte{0xde}};
    check(co_await op::write_some{file, bytes, 7} == 4, "write count");
    std::array<std::byte, 2> read{};
    check(co_await op::read_some{file, read, 8} == 2, "read count");
    check(
        read[0] == bytes[1] && read[1] == bytes[2], "file offset ignored");
    check(co_await op::read_some{file, read, 100} == 0, "EOF count");
    // Reuse after EOF catches an erroneously retained/duplicate packet.
    check(co_await op::read_some{file, read, 10} == 1, "short read count");
    check(read[0] == bytes[3], "short read content");
    bool rejected = false;
    try {
        (void) co_await op::read_some{file, read};
    } catch (invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "implicit file offset accepted");
}

task<void> send_bytes(SOCKET socket, std::span<const std::byte> bytes)
{
    while (!bytes.empty()) {
        auto n = co_await op::send_some{socket, bytes};
        check(n > 0 && n <= bytes.size(), "invalid send count");
        bytes = bytes.subspan(n);
    }
}

task<void> receive_bytes(SOCKET socket, std::span<std::byte> bytes)
{
    while (!bytes.empty()) {
        auto n = co_await op::recv_some{socket, bytes};
        check(n > 0 && n <= bytes.size(), "invalid receive count");
        bytes = bytes.subspan(n);
    }
}

constexpr auto payload = std::array{
    std::byte{1},
    std::byte{17},
    std::byte{42},
    std::byte{0},
    std::byte{255},
    std::byte{9},
    std::byte{73}};

task<void> echo_server(SOCKET listener)
{
    auto peer = socket_owner{co_await op::accept{listener}};
    std::array<std::byte, payload.size()> bytes{};
    co_await receive_bytes(peer.value, bytes);
    check(bytes == payload, "server payload differs");
    // Different chunk boundaries and a half-close exercise short I/O and
    // EOF.
    co_await send_bytes(peer.value, std::span{bytes}.first(3));
    co_await send_bytes(peer.value, std::span{bytes}.subspan(3));
    check(shutdown(peer.value, SD_SEND) == 0, "shutdown failed");
}

task<void> echo_client(iocp_wand & w, sockaddr_in address)
{
    auto socket = socket_owner{};
    w.attach(socket.value);
    co_await op::connect::from(
        socket.value,
        reinterpret_cast<sockaddr *>(&address),
        sizeof(address));
    // SO_UPDATE_CONNECT_CONTEXT must have been applied, not just the CQ
    // drained.
    sockaddr_in peer{};
    int length = sizeof(peer);
    check(
        getpeername(
            socket.value, reinterpret_cast<sockaddr *>(&peer), &length)
            == 0,
        "connect context missing");
    check(peer.sin_port == address.sin_port, "wrong peer port");
    co_await send_bytes(socket.value, payload);
    std::array<std::byte, payload.size()> bytes{};
    co_await receive_bytes(socket.value, bytes);
    check(bytes == payload, "echo payload differs");
    check(
        co_await op::recv_some{socket.value, bytes} == 0,
        "socket EOF count");
}

task<void> network(iocp_wand & w, listener & server)
{
    co_await when_all(
        echo_server(server.socket.value), echo_client(w, server.address));
}

task<std::size_t>
pending_receive(SOCKET socket, std::span<std::byte> buffer)
{
    co_return co_await op::recv_some{socket, buffer};
}

void cancel_before_submission()
{
    auto w = iocp_wand{};
    auto d = deck{&w};
    std::array<std::byte, 1> bytes{};
    auto root = root_task{
        d, [&] { return pending_receive(INVALID_SOCKET, bytes); }};
    root.inner().request_stop();
    root.start();
    w.run_until_done(d, root.inner());
    bool cancelled = false;
    try {
        (void) std::move(root.inner()).result();
    } catch (operation_cancelled const &) {
        cancelled = true;
    }
    check(cancelled, "queued stop submitted the invalid socket instead");
}

task<SOCKET> pending_accept(SOCKET listener)
{
    co_return co_await op::accept{listener};
}

void cancel_accept()
{
    auto server = listener{};
    auto w = iocp_wand{};
    w.attach(server.socket.value);
    auto d = deck{&w};
    auto root =
        root_task{d, [&] { return pending_accept(server.socket.value); }};
    root.start();
    d.run_ready();
    root.inner().request_stop();
    w.run_until_done(d, root.inner());
    bool cancelled = false;
    try {
        (void) std::move(root.inner()).result();
    } catch (operation_cancelled const &) {
        cancelled = true;
    }
    check(cancelled, "AcceptEx did not cancel");
    w.poll(d); // retire/close the unused accepted socket
    auto next = root_task{d, [&] { return network(w, server); }};
    next.start();
    w.run_until_done(d, next.inner());
    std::move(next.inner()).result();
}

void cancel_receive()
{
    auto pair = connected_pair{};
    auto w = iocp_wand{};
    w.attach(pair.peer.value);
    auto d = deck{&w};
    std::array<std::byte, 8> bytes{};
    auto root = root_task{
        d, [&] { return pending_receive(pair.peer.value, bytes); }};
    root.start();
    d.run_ready(); // submit a receive with no data available
    root.inner().request_stop();
    check(!root.inner().done(), "cancel settled before kernel completion");
    w.run_until_done(d, root.inner());
    bool cancelled = false;
    try {
        (void) std::move(root.inner()).result();
    } catch (operation_cancelled const &) {
        cancelled = true;
    }
    check(cancelled, "pending receive did not cancel");
    w.poll(d);
    // A new operation on the same handle must not see a stale cancellation.
    check(send(pair.client.value, "Q", 1, 0) == 1, "fixture send failed");
    auto next = root_task{
        d, [&] { return pending_receive(pair.peer.value, bytes); }};
    next.start();
    w.run_until_done(d, next.inner());
    check(
        std::move(next.inner()).result() == 1 && bytes[0] == std::byte{'Q'},
        "reuse after cancel failed");
}

void success_during_cancel()
{
    auto pair = connected_pair{};
    auto w = iocp_wand{};
    w.attach(pair.peer.value);
    auto d = deck{&w};
    std::array<std::byte, 8> bytes{};
    check(send(pair.client.value, "abc", 3, 0) == 3, "fixture send failed");
    // Wait for the fixture's data before submitting, so WSARecv completes
    // synchronously but its IOCP packet has not yet been consumed.
    char probe[3];
    check(
        recv(pair.peer.value, probe, 3, MSG_PEEK | MSG_WAITALL) == 3,
        "fixture data not ready");
    auto root = root_task{
        d, [&] { return pending_receive(pair.peer.value, bytes); }};
    root.start();
    d.run_ready();
    root.inner().request_stop();
    w.run_until_done(d, root.inner());
    check(
        std::move(root.inner()).result() == 3,
        "stop discarded successful receive");
    check(
        bytes[0] == std::byte{'a'} && bytes[2] == std::byte{'c'},
        "successful bytes lost");
}

task<void> unsupported()
{
    bool rejected = false;
    try {
        (void) co_await op::poll{};
    } catch (runtime_error const &) {
        rejected = true;
    }
    check(rejected, "readiness polling not rejected");
    bool failed = false;
    std::array<std::byte, 1> bytes{};
    try {
        (void) co_await op::recv_some{INVALID_SOCKET, bytes};
    } catch (std::system_error const &) {
        failed = true;
    }
    check(failed, "invalid socket succeeded");
}
} // namespace

int main()
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return 1;
    int failures = 0;
    auto test = [&](const char * name, auto fn) {
        try {
            fn();
            std::printf("PASS %s\n", name);
        } catch (std::exception const & e) {
            std::printf("FAIL %s: %s\n", name, e.what());
            ++failures;
        }
        std::fflush(stdout);
    };
    test("manual and monotonic timers", [] { run_with_iocp(timers); });
    test("timer cancellation", cancel_timer);
    test("overlapped file offsets, EOF and reuse", [] {
        auto file = file_owner{};
        auto w = iocp_wand{};
        w.attach(file.value);
        auto d = deck{&w};
        auto root = root_task{d, [&] { return files(file.value); }};
        root.start();
        w.run_until_done(d, root.inner());
        std::move(root.inner()).result();
    });
    test("ConnectEx/AcceptEx and socket I/O", [] {
        auto server = listener{};
        auto w = iocp_wand{};
        w.attach(server.socket.value);
        auto d = deck{&w};
        auto root = root_task{d, [&] { return network(w, server); }};
        root.start();
        w.run_until_done(d, root.inner());
        std::move(root.inner()).result();
    });
    test("CancelIoEx drain and socket reuse", cancel_receive);
    test("cancellation before submission", cancel_before_submission);
    test("AcceptEx cancellation and listener reuse", cancel_accept);
    test("successful completion wins cancellation", success_during_cancel);
    test("unsupported wishes and native errors", [] {
        run_with_iocp(unsupported);
    });
    WSACleanup();
    std::printf("IOCP: 9 tests, %d failures\n", failures);
    return failures ? 1 : 0;
}
