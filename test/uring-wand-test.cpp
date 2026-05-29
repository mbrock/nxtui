#include <nxtrt/buffers.hpp>
#include <nxtrt/app.hpp>
#include <nxtrt/fs.hpp>
#include <nxtrt/scoped_process.hpp>
#include <nxtrt/subprocess.hpp>
#include <nxtrt/uring_wand.hpp>
#include <nxt/unique-fd.hpp>
#include <nxtai/tool_process.hpp>

#include "test.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace nxt::test {

using namespace nxtui;

using namespace boost::ut;

nxtrt::task<std::string> echo_over_socketpair(int tx, int rx)
{
    auto message = std::string_view{"socket wish smoke"};
    auto sent = co_await nxtrt::send_some(tx, nxtrt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short socket send"};

    auto storage = std::array<std::byte, 64>{};
    auto source = nxtrt::socket_source{rx, std::span{storage}};
    auto chunk = co_await source.take_some();
    if (!chunk)
        throw std::runtime_error{"socket recv reached eof"};

    co_return std::string{nxtrt::as_string_view(*chunk)};
}

nxtrt::task<void> poll_after_socket_send(int tx, int rx)
{
    auto message = std::string_view{"x"};
    auto sent = co_await nxtrt::send_some(tx, nxtrt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short poll smoke send"};

    auto revents = co_await nxtrt::op::poll{
        .fd = rx,
        .events = POLLIN,
    };
    if ((revents & POLLIN) == 0)
        throw std::runtime_error{"poll did not report readable socket"};
}

nxtrt::task<void> timeout_once()
{
    co_await nxtrt::op::timeout::after(1ms);
}

nxtrt::task<int> timeout_value(int value)
{
    co_await nxtrt::op::timeout::after(1ms);
    co_return value;
}

nxtrt::task<std::vector<int>> many_short_timeouts()
{
    auto deeds = co_await nxtrt::with_zone(
        nxtrt::stop_on_failure{},
        [](auto & policy)
            -> nxtrt::task<std::vector<nxtrt::catching_deed<int>>> {
            auto out = std::vector<nxtrt::catching_deed<int>>{};
            out.reserve(32);
            for (auto i = 0; i != 32; ++i)
                out.push_back(policy.fork(timeout_value(i)).cope());
            co_return out;
        });

    auto values = std::vector<int>{};
    values.reserve(deeds.size());
    for (auto & deed : deeds) {
        auto result = std::move(deed).get();
        if (!result)
            nxtrt::rethrow(result.error());
        values.push_back(*result);
    }
    co_return values;
}

nxtrt::task<int> app_child_value(int value)
{
    co_await nxtrt::yield();
    co_return value;
}

nxtrt::task<void> poll_until_after_socket_send(int tx, int rx)
{
    auto message = std::string_view{"y"};
    auto sent = co_await nxtrt::send_some(tx, nxtrt::as_bytes(message));
    if (sent != message.size())
        throw std::runtime_error{"short poll-until smoke send"};

    auto result = co_await nxtrt::poll_until_after(rx, POLLIN, 1s);
    if (result.timed_out || (result.events & POLLIN) == 0)
        throw std::runtime_error{"poll-until did not report readable socket"};
}

nxtrt::task<void> poll_until_timeout(int rx)
{
    auto result = co_await nxtrt::poll_until_after(rx, POLLIN, 1ms);
    if (!result.timed_out)
        throw std::runtime_error{"poll-until did not time out"};
}

nxtrt::task<void> poll_forever(int rx)
{
    (void)co_await nxtrt::op::poll{
        .fd = rx,
        .events = POLLIN,
    };
}

nxtrt::task<void> poll_with_timeout(int rx)
{
    co_await nxtrt::with_timeout(1ms, poll_forever(rx));
}

nxtrt::task<void> poll_after_send_with_timeout(int tx, int rx)
{
    co_await nxtrt::with_timeout(1s, poll_after_socket_send(tx, rx));
}

nxtrt::task<void> poll_until_stopped(int rx)
{
    try {
        (void)co_await nxtrt::op::poll{
            .fd = rx,
            .events = POLLIN,
        };
    } catch (const nxtrt::operation_cancelled &) {
        co_return;
    }

    throw std::runtime_error{"poll completed instead of being cancelled"};
}

nxtrt::task<void> connect_to(int fd, sockaddr_in address)
{
    co_await nxtrt::op::connect::from(
        fd,
        reinterpret_cast<sockaddr const *>(&address),
        sizeof(address));
}

nxtrt::task<nxt::unique_fd> accept_one(int listener)
{
    co_return nxt::unique_fd{co_await nxtrt::op::accept{.fd = listener}};
}

nxtrt::task<struct statx> stat_current_directory()
{
    co_return co_await nxtrt::op::statx{
        .path = ".",
        .mask = STATX_TYPE,
    };
}

struct linux_dirent64
{
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

nxtrt::task<std::vector<std::string>> read_current_directory_names()
{
    auto fd = co_await nxtrt::op::openat{
        .path = ".",
        .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
    };
    auto dir = nxt::unique_fd{fd};

    auto storage = std::array<std::byte, 4096>{};
    auto bytes = co_await nxtrt::op::getdents64{
        .fd = dir.get(),
        .buffer = storage,
    };

    auto names = std::vector<std::string>{};
    for (auto offset = std::size_t{}; offset < bytes;) {
        auto const * entry = reinterpret_cast<linux_dirent64 const *>(
            storage.data() + offset);
        if (entry->d_reclen == 0)
            throw std::runtime_error{"getdents64 returned a zero-length entry"};

        names.emplace_back(entry->d_name);
        offset += entry->d_reclen;
    }
    co_return names;
}

nxtrt::task<void> write_to_fd(int fd, std::string_view text)
{
    auto written = co_await nxtrt::op::write_some{
        .fd = fd,
        .buffer = nxtrt::as_bytes(text),
    };
    if (written != text.size())
        throw std::runtime_error{"short write wish"};
}

nxtrt::task<std::size_t> write_with_fd_sink_count(int fd, std::string_view text)
{
    auto sink = nxtrt::fd_sink{fd, std::size_t{4}};
    co_await sink.write(text);
    co_await sink.write(std::string_view{"!"});
    co_await sink.flush();
    co_return sink.written_size();
}

nxtrt::task<nxtai::tool_process::result> capture_shell(
    std::string command,
    std::size_t cap_bytes = 64 * 1024)
{
    auto argv = std::vector<std::string>{};
    argv.emplace_back("/bin/sh");
    argv.emplace_back("-c");
    argv.push_back(std::move(command));
    co_return co_await nxtai::tool_process::capture(
        std::move(argv), cap_bytes);
}

nxtrt::task<nxtrt::child_result> terminate_sleeping_shell()
{
    auto argv = std::vector<std::string>{};
    argv.emplace_back("/bin/sh");
    argv.emplace_back("-c");
    argv.emplace_back("sleep 10");
    auto child = co_await nxtrt::op::spawn_piped{.argv = std::move(argv)};

    co_await nxtrt::op::signal_child{
        .pidfd = child.pid_fd(),
        .signal = SIGTERM,
    };
    co_return co_await nxtrt::op::wait_child{.pidfd = child.pid_fd()};
}

nxtrt::task<nxtrt::child_result> run_shell_in_pty()
{
    auto child = co_await nxtrt::op::spawn_pty{
        .argv = {"/bin/sh", "-c", "printf pty-ok"},
        .columns = 40,
        .rows = 8,
    };

    auto storage = std::array<std::byte, 128>{};
    auto output = std::string{};
    while (true) {
        try {
            auto n = co_await nxtrt::op::read_some{
                .fd = child.master_fd(),
                .buffer = std::span{storage},
            };
            if (n == 0)
                break;
            output += nxtrt::as_string_view(std::span{storage}.first(n));
        } catch (const nxtrt::runtime_error &) {
            break;
        }
    }
    child.master.reset();
    if (output.find("pty-ok") == std::string::npos)
        throw std::runtime_error{"pty did not carry child output"};

    co_return co_await nxtrt::subprocess::wait_child(child);
}

nxtrt::task<nxtrt::child_result> cleanup_sleeping_shell_after_cancel(
    bool & spawned)
{
    auto argv = std::vector<std::string>{};
    argv.emplace_back("/bin/sh");
    argv.emplace_back("-c");
    argv.emplace_back("sleep 10");
    auto child = co_await nxtrt::subprocess::spawn_piped(std::move(argv));
    spawned = true;

    auto cancelled = false;
    try {
        co_await nxtrt::op::timeout::after(10s);
    } catch (const nxtrt::operation_cancelled &) {
        cancelled = true;
    }

    if (!cancelled)
        throw std::runtime_error{"sleeping shell cleanup was not cancelled"};
    co_return co_await nxtrt::subprocess::terminate_and_wait(child, 100ms);
}

template<typename T>
T pump_until_done(
    nxtrt::deck & deck,
    nxtrt::uring_wand & wand,
    nxtrt::task<T> & task)
{
    wand.run_until_done(deck, task);
    return std::move(task).result();
}

void pump_until_done(
    nxtrt::deck & deck,
    nxtrt::uring_wand & wand,
    nxtrt::task<void> & task)
{
    wand.run_until_done(deck, task);
    std::move(task).result();
}

sockaddr_in loopback_listener_address(int fd)
{
    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (::bind(
            fd,
            reinterpret_cast<sockaddr const *>(&address),
            sizeof(address)) != 0)
        throw std::runtime_error{"bind failed"};
    if (::listen(fd, 1) != 0)
        throw std::runtime_error{"listen failed"};

    auto size = socklen_t{sizeof(address)};
    if (::getsockname(
            fd,
            reinterpret_cast<sockaddr *>(&address),
            &size) != 0)
        throw std::runtime_error{"getsockname failed"};

    return address;
}

static suite uring_wand_tests{
    "uring wand", [] {
        "runner"_test = [] {
            "runs task factories"_test = [] {
                auto value = nxtrt::run([]() -> nxtrt::task<int> {
                    co_await nxtrt::op::manual{};
                    co_return 42;
                });

                expect(value == 42_i);
            };

            "runtime owns a root zone and app channels"_test = [] {
                auto rt = nxtrt::runtime{};

                auto child = rt.run([]() -> nxtrt::task<nxtrt::deed<int>> {
                    expect(nxtrt::current_zone() != nullptr);
                    co_return nxtrt::fork(app_child_value(41));
                });

                expect(std::move(child).get() == 41_i);

                auto key = nxtui::input::KeyEvent{};
                key.key = nxtui::input::Key::character;
                key.text = "x";

                auto input_text = rt.run(
                    [&rt, key = std::move(key)]() mutable
                        -> nxtrt::task<std::string> {
                        expect(co_await rt.publish_input_event(
                            std::move(key)));
                        auto event = co_await rt.next_input();
                        co_return event ? event->text : std::string{};
                    });

                expect(input_text == "x");

                auto resized = rt.publish_resize(
                    nxtui::Size{80 * nxtui::ch, 24 * nxtui::ln});
                expect(resized);
                auto size = rt.next_resize_now();
                expect(size.has_value());
                expect(size->w == 80 * nxtui::ch);
                expect(size->h == 24 * nxtui::ln);
            };

            "runtime sleeps on its platform wand"_test = [] {
                auto rt = nxtrt::runtime{};

                rt.run([&rt]() -> nxtrt::task<void> {
                    co_await rt.sleep(1ms);
                });
            };
        };

        "socket I/O"_test = [] {
            "echoes over a socketpair"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = echo_over_socketpair(first.get(), second.get());

                deck.start(task);
                auto echoed = pump_until_done(deck, wand, task);

                expect(echoed == "socket wish smoke");
            };

            "socket sends complete before readability is polled"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = poll_after_socket_send(first.get(), second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "loopback listeners accept connected clients"_test = [] {
                auto listener = nxt::unique_fd{::socket(AF_INET, SOCK_STREAM, 0)};
                if (listener.get() < 0)
                    throw std::runtime_error{"listener socket failed"};
                auto address = loopback_listener_address(listener.get());

                auto client = nxt::unique_fd{::socket(AF_INET, SOCK_STREAM, 0)};
                if (client.get() < 0)
                    throw std::runtime_error{"client socket failed"};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = accept_one(listener.get());

                deck.start(task);
                if (::connect(
                        client.get(),
                        reinterpret_cast<sockaddr const *>(&address),
                        sizeof(address)) != 0)
                    throw std::runtime_error{"client connect failed"};

                auto accepted = pump_until_done(deck, wand, task);
                expect(accepted.get() >= 0);
            };
        };

        "file I/O"_test = [] {
            "statx wishes return file metadata"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = stat_current_directory();

                deck.start(task);
                auto stat = pump_until_done(deck, wand, task);

                expect((stat.stx_mask & STATX_TYPE) != 0);
                expect(S_ISDIR(stat.stx_mode));
            };

            "getdents64 wishes return directory entries"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = read_current_directory_names();

                deck.start(task);
                auto names = pump_until_done(deck, wand, task);

                expect(std::ranges::find(names, ".") != names.end());
                expect(std::ranges::find(names, "..") != names.end());
            };

            "fs lists portable directory entries"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = nxtrt::fs::list_path(".");

                deck.start(task);
                auto entries = pump_until_done(deck, wand, task);

                auto dot = std::ranges::find(
                    entries,
                    ".",
                    &nxtrt::fs::directory_entry::name);
                expect(dot != entries.end());
                expect(dot->status.kind == nxtrt::fs::file_kind::directory);
            };
        };

        "file descriptor I/O"_test = [] {
            "write wishes write to file descriptors"_test = [] {
                auto fds = std::array<int, 2>{-1, -1};
                if (::pipe(fds.data()) != 0)
                    throw std::runtime_error{"pipe failed"};

                auto rx = nxt::unique_fd{fds[0]};
                auto tx = nxt::unique_fd{fds[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = write_to_fd(tx.get(), "wishful stdout");

                deck.start(task);
                pump_until_done(deck, wand, task);

                auto buffer = std::array<char, 32>{};
                auto n = ::read(rx.get(), buffer.data(), buffer.size());
                if (n < 0)
                    throw std::runtime_error{"pipe read failed"};

                expect(std::string_view{buffer.data(), static_cast<std::size_t>(n)}
                    == "wishful stdout");
            };

            "fd sinks count flushed bytes"_test = [] {
                auto fds = std::array<int, 2>{-1, -1};
                if (::pipe(fds.data()) != 0)
                    throw std::runtime_error{"pipe failed"};

                auto rx = nxt::unique_fd{fds[0]};
                auto tx = nxt::unique_fd{fds[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = write_with_fd_sink_count(tx.get(), "counted");

                deck.start(task);
                auto counted = pump_until_done(deck, wand, task);

                auto buffer = std::array<char, 32>{};
                auto n = ::read(rx.get(), buffer.data(), buffer.size());
                if (n < 0)
                    throw std::runtime_error{"pipe read failed"};

                expect(counted == std::size_t{8});
                expect(std::string_view{buffer.data(), static_cast<std::size_t>(n)}
                    == "counted!");
            };

            "subprocess capture drains stdout and stderr"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = capture_shell(
                    "printf 'out'; printf 'err' >&2");

                deck.start(task);
                auto child = pump_until_done(deck, wand, task);

                expect(child.status.exited);
                expect(child.status.exit_code == 0_i);
                expect(child.output == "outerr");
                expect(!child.failed);
                expect(!child.output_too_large);
            };

            "subprocess capture records nonzero exits"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = capture_shell("printf 'nope'; exit 7");

                deck.start(task);
                auto child = pump_until_done(deck, wand, task);

                expect(child.status.exited);
                expect(child.status.exit_code == 7_i);
                expect(child.output == "nope");
                expect(!child.failed);
            };

            "subprocess capture does not inherit runtime stdin"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = capture_shell(
                    "if read line; then printf 'stdin:%s' \"$line\"; "
                    "else printf 'stdin-eof'; fi");

                deck.start(task);
                auto child = pump_until_done(deck, wand, task);

                expect(child.status.exited);
                expect(child.status.exit_code == 0_i);
                expect(child.output == "stdin-eof");
                expect(!child.failed);
            };

            "subprocess capture fails oversized output after draining"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = capture_shell("printf 'abcdefgh'; exit 7", 5);

                deck.start(task);
                auto child = pump_until_done(deck, wand, task);

                expect(child.status.exited);
                expect(child.status.exit_code == 7_i);
                expect(child.failed);
                expect(child.output_too_large);
                expect(child.failure_reason
                    == "tool output exceeded capture limit (5 bytes)");
                expect(child.output.find("abcde") != std::string::npos);
            };

            "systemd scope wrapping preserves child argv"_test = [] {
                auto argv = std::vector<std::string>{
                    "/bin/bash",
                    "-c",
                    "printf hi",
                };
                auto wrapped = nxtrt::scoped_process::systemd_scope_argv(
                    "nxt-test.scope",
                    std::move(argv));

                expect(wrapped[0] == "systemd-run");
                expect(wrapped[1] == "--user");
                expect(wrapped[2] == "--scope");
                expect(wrapped[5] == "--unit=nxt-test.scope");
                expect(wrapped[6] == "/bin/bash");
                expect(wrapped[7] == "-c");
                expect(wrapped[8] == "printf hi");
            };

            "subprocess children are signalled through pidfds"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = terminate_sleeping_shell();

                deck.start(task);
                auto status = pump_until_done(deck, wand, task);

                expect(status.signaled);
                expect(status.signal == SIGTERM);
            };

            "pty subprocesses run and wait through pidfds"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = run_shell_in_pty();

                deck.start(task);
                auto status = pump_until_done(deck, wand, task);

                expect(status.exited);
                expect(status.exit_code == 0_i);
            };

            "subprocess cleanup is shielded after task cancellation"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto spawned = false;
                auto task = cleanup_sleeping_shell_after_cancel(spawned);

                deck.start(task);
                for (auto i = 0; i != 8 && !spawned; ++i) {
                    if (!deck.empty())
                        deck.run_ready();
                    wand.wave(deck);
                    wand.poll(deck);
                }

                expect(spawned);
                task.request_stop();
                auto status = pump_until_done(deck, wand, task);

                expect(status.signaled);
                expect(status.signal == SIGTERM);
            };
        };

        "timers and polling"_test = [] {
            "timeout wishes complete"_test = [] {
                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = timeout_once();

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "pending wishes wait for submission queue capacity"_test = [] {
                auto wand = nxtrt::uring_wand{4};
                auto deck = nxtrt::deck{&wand};
                auto task = many_short_timeouts();

                deck.start(task);
                auto values = pump_until_done(deck, wand, task);

                expect(task.done());
                expect(values.size() == std::size_t{32});
            };

            "readiness is reported before a poll-until deadline"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task =
                    poll_until_after_socket_send(first.get(), second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "quiet watched fds time out"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = poll_until_timeout(second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "poll wishes are cancelled when their task stops"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = poll_until_stopped(second.get());

                deck.start(task);
                deck.run_ready();
                expect(!task.done());

                task.request_stop();
                wand.run_until_done(deck, task);
                std::move(task).result();
            };

            "with_timeout returns when the body wins"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task =
                    poll_after_send_with_timeout(first.get(), second.get());

                deck.start(task);
                pump_until_done(deck, wand, task);

                expect(task.done());
            };

            "with_timeout throws when the timer wins"_test = [] {
                auto sockets = std::array<int, 2>{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0)
                    throw std::runtime_error{"socketpair failed"};

                auto first = nxt::unique_fd{sockets[0]};
                auto second = nxt::unique_fd{sockets[1]};

                auto wand = nxtrt::uring_wand{};
                auto deck = nxtrt::deck{&wand};
                auto task = poll_with_timeout(second.get());

                deck.start(task);

                auto timed_out = false;
                try {
                    pump_until_done(deck, wand, task);
                } catch (const nxtrt::timeout_error &) {
                    timed_out = true;
                }

                expect(timed_out);
            };
        };
    }};

} // namespace nxt::test
