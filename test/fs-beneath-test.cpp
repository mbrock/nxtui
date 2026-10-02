#include <nxtrt/fs.hpp>

#include "test.hpp"

#include <filesystem>
#include <fstream>

namespace nxt::test {
namespace {

using namespace boost::ut;
namespace fs = nxtrt::fs;

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
    } catch (const std::system_error & error) {
        return error.code().value();
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

static suite beneath_tests{
    "paths beneath a directory", [] {
        "plain relative names resolve"_test = [] {
            auto t = tree{};
            expect(read_all(fs::open_beneath(t.root.get(), "a.txt")) == "a");
            expect(
                read_all(fs::open_beneath(t.root.get(), "sub/b.txt")) == "b");
            expect(S_ISDIR(fs::stat_beneath(t.root.get(), "").st_mode));
            expect(S_ISDIR(fs::stat_beneath(t.root.get(), "sub").st_mode));
            expect(
                fs::directory_names(fs::open_beneath(
                    t.root.get(), "", O_RDONLY | O_DIRECTORY))
                == std::vector<std::string>{
                    "a.txt", "fifo", "sub", "up"});
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
