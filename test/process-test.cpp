#include <nxtai/tool_process.hpp>
#include <nxtrt/subprocess.hpp>
#include <nxtrt/task/concurrent.hpp>

#if defined(__linux__)
#  include <nxtrt/wand/epoll.hpp>
#  include <nxtrt/wand/uring.hpp>
#else
#  include <nxtrt/wand/kqueue.hpp>
#endif

#include "test.hpp"

#include <array>
#include <csignal>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace nxt::test {
namespace {

using namespace boost::ut;
using namespace std::chrono_literals;
using nxtrt::task;
namespace subprocess = nxtrt::subprocess;

// Runs one root task on a fresh WAND and returns its result.
template<class Wand, class Fn>
auto run_on(Fn fn)
{
    auto wand = Wand{};
    auto deck = nxtrt::deck{&wand};
    auto root = nxtrt::root_task{deck, std::move(fn)};
    root.start();
    wand.run_until_done(deck, root.inner());
    return std::move(root.inner()).result();
}

task<std::string> read_all(int fd)
{
    auto storage = std::array<std::byte, 256>{};
    auto output = std::string{};
    while (true) {
        std::size_t n = 0;
        try {
            n = co_await nxtrt::op::read_some{fd, std::span{storage}};
        } catch (const nxtrt::errno_error &) {
            break; // a PTY master reports EIO once the slave closes
        }
        if (n == 0)
            break;
        output += nxtrt::as_string_view(std::span{storage}.first(n));
    }
    co_return output;
}

struct captured
{
    std::string output;
    nxtrt::child_result status;
    pid_t pid = -1;
};

task<captured> capture(std::vector<std::string> argv)
{
    auto child = co_await subprocess::spawn_piped(std::move(argv));
    auto output = co_await read_all(child.output_fd());
    auto status = co_await subprocess::wait_child(child);
    co_return captured{std::move(output), status, child.pid};
}

task<nxtrt::child_result> terminate_sleeper()
{
    auto child = co_await subprocess::spawn_piped({"/bin/sleep", "10"});
    co_return co_await subprocess::terminate_and_wait(child);
}

task<bool> wait_until_stopped(int child)
{
    try {
        (void) co_await nxtrt::op::wait_child{child};
    } catch (const nxtrt::operation_cancelled &) {
        co_return true;
    }
    co_return false;
}

task<std::vector<int>> wait_for_many(int count)
{
    auto children = std::vector<subprocess::piped_child>{};
    for (auto i = 0; i < count; ++i) {
        // GCC 15 ICEs when the initializer and await are inside push_back.
        auto argv = std::vector<std::string>{
            "/bin/sh", "-c", "exit " + std::to_string(i)};
        auto child = co_await subprocess::spawn_piped(std::move(argv));
        children.push_back(std::move(child));
    }
    auto codes = std::vector<int>{};
    for (auto & child : children)
        codes.push_back((co_await subprocess::wait_child(child)).exit_code);
    co_return codes;
}

task<nxtrt::child_result> wait_child_of(subprocess::piped_child & child)
{
    co_return co_await subprocess::wait_child(child);
}

task<std::string> pty_output()
{
    auto child = co_await nxtrt::op::spawn_pty{
        {"/bin/sh", "-c", "stty size; printf pty-ok; exit 4"}, 40, 8};
    auto output = co_await read_all(child.master_fd());
    child.master.reset();
    auto status = co_await subprocess::wait_child(child);
    co_return output + "|" + std::to_string(status.exit_code);
}

template<class Wand>
void process_tests()
{
    "piped children merge stdout and stderr and report exit codes"_test =
        [] {
            auto run = run_on<Wand>([] {
                return capture(
                    {"/bin/sh", "-c", "echo out; echo err >&2; exit 3"});
            });
            expect(run.output == "out\nerr\n");
            expect(run.status.exited && run.status.exit_code == 3_i);
            expect(run.status.pid == run.pid);
            // Exited children are reaped once their handle is gone.
            expect(::waitpid(run.pid, nullptr, WNOHANG) == -1_i);
            expect(errno == ECHILD);
        };
    "signals reach running children through their handle"_test = [] {
        auto status = run_on<Wand>([] { return terminate_sleeper(); });
        expect(status.signaled && status.signal == SIGTERM);
    };
    "missing programs fail to spawn"_test = [] {
        auto failed = false;
        try {
            (void) run_on<Wand>([] {
                return subprocess::spawn_piped({"/nonexistent/program"});
            });
        } catch (const nxtrt::errno_error & error) {
            failed = error.code() == ENOENT;
        }
        expect(failed);
    };
    "an already exited child completes its wait"_test = [] {
        auto child = run_on<Wand>(
            [] { return subprocess::spawn_piped({"/bin/sh", "-c", "exit 9"}); });
        auto info = siginfo_t{};
        expect(
            ::waitid(P_PID, id_t(child.pid), &info, WEXITED | WNOWAIT) == 0_i);
        auto status = run_on<Wand>([&] { return wait_child_of(child); });
        expect(status.exited && status.exit_code == 9_i);
    };
    "cancelled waits leave children waitable"_test = [] {
        auto child = run_on<Wand>(
            [] { return subprocess::spawn_piped({"/bin/sleep", "10"}); });
        auto wand = Wand{};
        auto deck = nxtrt::deck{&wand};
        auto waiting = nxtrt::root_task{
            deck, [&] { return wait_until_stopped(child.child_ref()); }};
        waiting.start();
        deck.run_ready();
        expect(!waiting.inner().done());
        waiting.inner().request_stop();
        wand.run_until_done(deck, waiting.inner());
        expect(std::move(waiting.inner()).result());
        auto cleanup = nxtrt::root_task{
            deck, [&] { return subprocess::terminate_and_wait(child); }};
        cleanup.start();
        wand.run_until_done(deck, cleanup.inner());
        auto status = std::move(cleanup.inner()).result();
        expect(status.signaled && status.signal == SIGTERM);
    };
    "many children complete independently"_test = [] {
        auto codes = run_on<Wand>([] { return wait_for_many(24); });
        expect(codes.size() == std::size_t{24});
        for (auto i = 0; i < int(codes.size()); ++i)
            expect(codes[std::size_t(i)] == i);
    };
    "PTY children see their terminal size"_test = [] {
        auto output = run_on<Wand>([] { return pty_output(); });
        expect(output.find("8 40") != std::string::npos) << output;
        expect(output.find("pty-ok") != std::string::npos) << output;
        expect(output.ends_with("|4")) << output;
    };
    "tool capture runs shell commands"_test = [] {
        auto result = run_on<Wand>([] {
            return nxtai::tool_process::capture(
                {"/bin/sh", "-c", "printf captured; exit 2"});
        });
        expect(result.output == "captured");
        expect(result.status.exit_code == 2_i);
    };
#if defined(__APPLE__)
    "children inherit only their standard streams"_test = [] {
        // POSIX_SPAWN_CLOEXEC_DEFAULT: even descriptors without FD_CLOEXEC
        // stay in the parent.
        auto fds = std::array<int, 2>{};
        expect(::pipe(fds.data()) == 0_i);
        auto read = nxt::unique_fd{fds[0]}, write = nxt::unique_fd{fds[1]};
        auto probe = "test -e /dev/fd/" + std::to_string(write.get())
                     + " && echo leaked || echo closed";
        auto run =
            run_on<Wand>([&] { return capture({"/bin/sh", "-c", probe}); });
        expect(run.output == "closed\n") << run.output;
    };
#endif
}

#if defined(__linux__)
static suite process_epoll_tests{
    "processes on epoll", process_tests<nxtrt::epoll_wand>};
#if NXT_RT_HAS_URING
static suite process_uring_tests{
    "processes on uring", process_tests<nxtrt::uring_wand>};
#endif
#else
static suite process_kqueue_tests{
    "processes on kqueue", process_tests<nxtrt::kqueue_wand>};
#endif

} // namespace
} // namespace nxt::test
