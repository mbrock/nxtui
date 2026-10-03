#include <nxtrt/fs.hpp>
#include <nxtrt/task/concurrent.hpp>

#if defined(__linux__)
#  include <nxtrt/wand/epoll.hpp>
#  include <nxtrt/wand/uring.hpp>
#else
#  include <nxtrt/wand/kqueue.hpp>
#endif

#include "test.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

namespace nxt::test {
namespace {

using namespace boost::ut;
using namespace std::chrono_literals;
namespace fs = nxtrt::fs;
using nxtrt::task;

// root/ holds a.txt, sub/b.txt, links inside and outside, and a FIFO.
struct tree
{
    std::filesystem::path base;
    nxt::unique_fd root;

    tree()
    {
        base = std::filesystem::temp_directory_path()
               / ("nxt-beneath-" + std::to_string(::getpid()));
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base / "root/sub");
        std::ofstream{base / "secret"} << "outside";
        std::ofstream{base / "root/a.txt"} << "a";
        std::ofstream{base / "root/sub/b.txt"} << "b";
        std::filesystem::create_symlink("../a.txt", base / "root/sub/inner");
        std::filesystem::create_symlink("..", base / "root/up");
        ::mkfifo((base / "root/fifo").c_str(), 0600);
        root = nxt::unique_fd{::open(
            (base / "root").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    }

    ~tree()
    {
        std::filesystem::remove_all(base);
    }
};

int error_of(auto operation)
{
    try {
        operation();
    } catch (const nxtrt::errno_error & error) {
        return error.code();
    } catch (const std::invalid_argument &) {
        return EINVAL;
    }
    return 0;
}

std::string read_all(nxt::unique_fd fd)
{
    auto buffer = std::array<char, 64>{};
    auto count = ::read(fd.get(), buffer.data(), buffer.size());
    return {buffer.data(), std::size_t(std::max<ssize_t>(count, 0))};
}

template<typename T>
task<int> errno_of(task<T> work)
{
    try {
        (void) co_await std::move(work);
    } catch (const nxtrt::errno_error & error) {
        co_return error.code();
    } catch (const std::invalid_argument &) {
        co_return EINVAL;
    }
    co_return 0;
}

// The same answers whether the wand performs file operations (io_uring) or
// they run on the blocking pool (epoll, kqueue).
task<void> files_agree(fs::files & io, int root)
{
    auto fd = co_await io.open_beneath(root, "sub/b.txt");
    auto buffer = std::array<std::byte, 8>{};
    expect(co_await io.read_at(fd.get(), buffer, 0) == 1u);
    expect(char(buffer[0]) == 'b');
    auto file = co_await io.stat_beneath(root, "a.txt");
    expect(file.kind == fs::file_kind::regular);
    expect(file.size == 1u);
    expect(file.modified_ms > 1'600'000'000'000);
    expect((co_await io.stat_beneath(root, "")).kind
           == fs::file_kind::directory);
    expect((co_await io.stat_beneath(root, "sub")).kind
           == fs::file_kind::directory);
    expect((co_await io.stat_beneath(root, "up")).kind
           == fs::file_kind::symlink);
    expect(co_await errno_of(io.open_beneath(root, "sub/inner")) == ELOOP);
    expect(co_await errno_of(io.open_beneath(root, "up/secret")) == ELOOP);
    expect(co_await errno_of(io.stat_beneath(root, "up/root")) == ELOOP);
    expect(co_await errno_of(io.open_beneath(root, "missing")) == ENOENT);
    expect(co_await errno_of(io.stat_beneath(root, "missing")) == ENOENT);
    expect(co_await errno_of(io.open_beneath(root, "../secret")) == EINVAL);
    auto entries = co_await io.list_beneath(root, "sub");
    expect(entries.size() == 2u);
    expect(entries[0].name == "b.txt");
    expect(entries[0].status.size == 1u);
    expect(entries[1].name == "inner");
    expect(entries[1].status.kind == fs::file_kind::symlink);
}

// A blocking FIFO open waits for a writer. Only a deck that keeps running
// while it waits can fire the timer that opens the writer end. io_uring's
// first open attempt is non-blocking, so there a read-only FIFO open
// returns at once instead of waiting: IMMEDIATE.
task<void> open_fifo_reader(fs::files & io, int root, bool & opened)
{
    auto fd = co_await io.open_beneath(root, "fifo", O_RDONLY);
    opened = true;
}

task<void> open_fifo_writer(
    std::string path,
    bool & reader_opened,
    std::atomic<bool> & from_deck,
    bool & immediate)
{
    co_await nxtrt::op::timeout::after(1ms);
    immediate = reader_opened;
    for (auto attempt = 0; attempt < 2000 && !reader_opened; ++attempt) {
        co_await nxtrt::op::timeout::after(1ms);
        auto fd = nxt::unique_fd{::open(path.c_str(), O_WRONLY | O_NONBLOCK)};
        if (fd.get() >= 0) {
            from_deck = true;
            while (!reader_opened)
                co_await nxtrt::op::timeout::after(1ms);
        }
    }
}

template<class Wand>
void files_tests()
{
    "files resolve beneath a directory like the synchronous calls"_test =
        [] {
            auto t = tree{};
            auto workers = nxtrt::blocking_pool{2, 8};
            auto io = fs::files{workers};
            auto wand = Wand{};
            auto deck = nxtrt::deck{&wand};
            auto root = nxtrt::root_task{
                deck, [&] { return files_agree(io, t.root.get()); }};
            root.start();
            wand.run_until_done(deck, root.inner());
            std::move(root.inner()).result();
        };
    "a blocking open leaves the deck running"_test = [] {
        auto t = tree{};
        auto workers = nxtrt::blocking_pool{2, 8};
        auto io = fs::files{workers};
        auto wand = Wand{};
        auto deck = nxtrt::deck{&wand};
        auto opened = false;
        auto immediate = false;
        auto from_deck = std::atomic<bool>{false};
        auto fifo = (t.base / "root/fifo").string();
        // Rescue a stuck deck so a failure is reported instead of hanging.
        auto rescue = std::jthread{[&](std::stop_token stop) {
            for (auto i = 0; i < 50 && !stop.stop_requested(); ++i)
                std::this_thread::sleep_for(10ms);
            if (!stop.stop_requested())
                ::close(::open(fifo.c_str(), O_WRONLY | O_NONBLOCK));
        }};
        auto root = nxtrt::root_task{deck, [&] {
            return nxtrt::when_all(
                open_fifo_reader(io, t.root.get(), opened),
                open_fifo_writer(fifo, opened, from_deck, immediate));
        }};
        root.start();
        wand.run_until_done(deck, root.inner());
        rescue.request_stop();
        std::move(root.inner()).result();
        expect(opened);
        expect(immediate || from_deck.load())
            << "immediate" << immediate << "from_deck" << from_deck.load();
    };
#if defined(__linux__) && NXT_RT_HAS_URING
    if constexpr (std::same_as<Wand, nxtrt::uring_wand>) {
        "directory metadata stays bounded with more entries than task slots"_test =
            [] {
                auto t = tree{};
                for (auto i = 0; i != 256; ++i)
                    std::ofstream{
                        t.base / "root" / ("entry-" + std::to_string(i))}
                        << i;
                auto wand = Wand{};
                auto task_land = nxtrt::static_deck_task_storage<128>{};
                auto deck = nxtrt::deck{task_land, &wand};
                auto root =
                    nxtrt::root_task{deck, [&] {
                                         return fs::list_directory(
                                             (t.base / "root").string());
                                     }};
                root.start();
                wand.run_until_done(deck, root.inner());
                auto entries = std::move(root.inner()).result();
                expect(
                    entries.size()
                    == 262u); // 256 files, four fixtures, . and ..
                expect(
                    std::ranges::is_sorted(
                        entries, {}, &fs::directory_entry::name));
                expect(entries[0].name == ".");
                expect(entries[1].name == "..");
                auto link = std::ranges::find(
                    entries, "up", &fs::directory_entry::name);
                expect(link != entries.end());
                if (link != entries.end())
                    expect(link->status.kind == fs::file_kind::symlink);
                auto file = std::ranges::find(
                    entries, "entry-123", &fs::directory_entry::name);
                expect(file != entries.end());
                if (file != entries.end())
                    expect(file->status.size == 3u);
            };
    }
#endif
}

#if defined(__linux__)
static suite files_epoll_tests{"files on epoll", files_tests<nxtrt::epoll_wand>};
#if NXT_RT_HAS_URING
static suite files_uring_tests{"files on uring", files_tests<nxtrt::uring_wand>};
#endif
#else
static suite files_kqueue_tests{
    "files on kqueue", files_tests<nxtrt::kqueue_wand>};
#endif

static suite beneath_tests{
    "paths beneath a directory", [] {
        "plain relative names resolve"_test = [] {
            auto t = tree{};
            expect(read_all(fs::open_beneath(t.root.get(), "a.txt")) == "a");
            expect(
                read_all(fs::open_beneath(t.root.get(), "sub/b.txt")) == "b");
            expect(S_ISDIR(fs::stat_beneath(t.root.get(), "").st_mode));
            expect(S_ISDIR(fs::stat_beneath(t.root.get(), "sub").st_mode));
            auto names = std::vector<std::string>{};
            for (auto & entry : fs::list_entries(fs::open_beneath(
                     t.root.get(), "", O_RDONLY | O_DIRECTORY)))
                names.push_back(entry.name);
            expect(
                names
                == std::vector<std::string>{"a.txt", "fifo", "sub", "up"});
        };
        "malformed segments are rejected"_test = [] {
            auto t = tree{};
            for (auto path :
                 {"/a.txt", "../secret", "sub/../a.txt", "./a.txt", "sub//b.txt",
                  "sub/", "a.txt/."})
                expect(
                    error_of([&] { (void) fs::open_beneath(t.root.get(), path); })
                    == EINVAL)
                    << path;
        };
        "symlinks are never followed"_test = [] {
            auto t = tree{};
            // Even a link that stays beneath is refused, final or not.
            expect(
                error_of([&] {
                    (void) fs::open_beneath(t.root.get(), "sub/inner");
                })
                == ELOOP);
            expect(
                error_of([&] {
                    (void) fs::open_beneath(t.root.get(), "up/secret");
                })
                == ELOOP);
            expect(
                error_of([&] {
                    (void) fs::stat_beneath(t.root.get(), "up/root/a.txt");
                })
                == ELOOP);
            expect(S_ISLNK(fs::stat_beneath(t.root.get(), "up").st_mode));
        };
        "missing entries and FIFOs"_test = [] {
            auto t = tree{};
            expect(
                error_of([&] {
                    (void) fs::open_beneath(t.root.get(), "missing");
                })
                == ENOENT);
            expect(
                error_of([&] {
                    (void) fs::open_beneath(t.root.get(), "a.txt/x");
                })
                == ENOTDIR);
            // Non-blocking open returns instead of waiting for a writer.
            auto fifo = fs::open_beneath(
                t.root.get(), "fifo", O_RDONLY | O_NONBLOCK);
            expect(S_ISFIFO(fs::stat_beneath(t.root.get(), "fifo").st_mode));
            expect(fifo.get() >= 0);
        };
    }};

} // namespace
} // namespace nxt::test
