#include <nxt/rt/buffers.hpp>
#include <nxt/rt/uring_wand.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

class unique_fd
{
public:
    explicit unique_fd(int fd = -1) noexcept
        : fd_(fd)
    {}

    unique_fd(const unique_fd &) = delete;
    unique_fd & operator=(const unique_fd &) = delete;

    unique_fd(unique_fd && other) noexcept
        : fd_(std::exchange(other.fd_, -1))
    {}

    unique_fd & operator=(unique_fd && other) noexcept
    {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~unique_fd()
    {
        close();
    }

    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }

private:
    void close() noexcept
    {
        if (fd_ >= 0)
            ::close(std::exchange(fd_, -1));
    }

    int fd_ = -1;
};

struct linux_dirent64
{
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

struct listing_entry
{
    std::string name;
    struct statx stat{};
};

class dirents_source final : public nxt::rt::byte_source
{
public:
    explicit dirents_source(int fd) noexcept
        : fd_(fd)
    {}

    nxt::rt::task<nxt::rt::read_result>
    read_some(std::span<std::byte> dst) override
    {
        auto n = co_await nxt::rt::getdents64_wish{
            .fd = fd_,
            .buffer = dst,
        };
        co_return nxt::rt::read_result{
            .bytes = n,
            .eof = n == 0,
        };
    }

private:
    int fd_ = -1;
};

std::string mode_string(mode_t mode)
{
    auto result = std::string{"----------"};

    if (S_ISDIR(mode))
        result[0] = 'd';
    else if (S_ISLNK(mode))
        result[0] = 'l';
    else if (S_ISCHR(mode))
        result[0] = 'c';
    else if (S_ISBLK(mode))
        result[0] = 'b';
    else if (S_ISFIFO(mode))
        result[0] = 'p';
    else if (S_ISSOCK(mode))
        result[0] = 's';

    constexpr auto bits = std::array{
        S_IRUSR, S_IWUSR, S_IXUSR,
        S_IRGRP, S_IWGRP, S_IXGRP,
        S_IROTH, S_IWOTH, S_IXOTH,
    };
    constexpr auto chars = std::array{
        'r', 'w', 'x',
        'r', 'w', 'x',
        'r', 'w', 'x',
    };

    for (auto i = std::size_t{}; i != bits.size(); ++i) {
        if ((mode & bits[i]) != 0)
            result[i + 1] = chars[i];
    }

    return result;
}

bool hidden_or_dot(std::string_view name)
{
    return name.empty() || name.front() == '.';
}

nxt::rt::task<std::vector<listing_entry>> list_directory(std::string path)
{
    auto fd = co_await nxt::rt::openat_wish{
        .dirfd = AT_FDCWD,
        .path = std::move(path),
        .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        .mode = 0,
    };
    auto dir = unique_fd{fd};
    auto source = dirents_source{dir.get()};
    auto storage = std::array<std::byte, 16 * 1024>{};
    auto reader = nxt::rt::byte_reader{source, std::span{storage}};

    auto entries = std::vector<listing_entry>{};
    while (auto chunk = co_await reader.take_some()) {
        for (auto offset = std::size_t{}; offset < chunk->size();) {
            auto const * entry = reinterpret_cast<linux_dirent64 const *>(
                chunk->data() + offset);
            if (entry->d_reclen == 0)
                throw std::runtime_error{
                    "getdents64 returned a zero-length entry"};

            auto name = std::string_view{entry->d_name};
            if (!hidden_or_dot(name)) {
                auto stat = co_await nxt::rt::statx_wish{
                    .dirfd = dir.get(),
                    .path = std::string{name},
                    .flags = AT_SYMLINK_NOFOLLOW,
                    .mask = STATX_TYPE | STATX_MODE | STATX_SIZE,
                };
                entries.push_back(
                    listing_entry{
                        .name = std::string{name},
                        .stat = stat,
                    });
            }

            offset += entry->d_reclen;
        }
    }

    std::ranges::sort(
        entries,
        {},
        &listing_entry::name);
    co_return entries;
}

nxt::rt::task<std::vector<listing_entry>> list_path(std::string path)
{
    auto stat = co_await nxt::rt::statx_wish{
        .dirfd = AT_FDCWD,
        .path = path,
        .flags = AT_SYMLINK_NOFOLLOW,
        .mask = STATX_TYPE | STATX_MODE | STATX_SIZE,
    };

    if (S_ISDIR(stat.stx_mode))
        co_return co_await list_directory(std::move(path));

    co_return std::vector<listing_entry>{
        listing_entry{
            .name = std::move(path),
            .stat = stat,
        },
    };
}

template<typename T>
void pump_until_done(
    nxt::rt::deck & deck,
    nxt::rt::uring_wand & wand,
    nxt::rt::task<T> & task)
{
    for (auto spins = 0; spins != 10000 && !task.done(); ++spins) {
        if (!deck.empty())
            deck.run_ready();
        wand.poll(deck);
        if (deck.empty() && !task.done())
            std::this_thread::sleep_for(1ms);
    }

    if (!task.done())
        throw std::runtime_error{"demo task did not complete"};
}

template<typename Sink>
nxt::rt::task<void> print_entries(
    nxt::rt::byte_writer<Sink> & writer,
    std::vector<listing_entry> const & entries)
{
    auto width = std::size_t{1};
    for (auto const & entry : entries)
        width = std::max(width, std::to_string(entry.stat.stx_size).size());

    for (auto const & entry : entries) {
        co_await writer.write(
            std::format(
                "{} {:>{}} {}\n",
                mode_string(entry.stat.stx_mode),
                entry.stat.stx_size,
                width,
                entry.name));
    }
    co_await writer.flush();
}

nxt::rt::task<void> list_and_print(std::string path)
{
    auto entries = co_await list_path(std::move(path));
    auto out = nxt::rt::fd_sink{STDOUT_FILENO};
    auto storage = std::array<std::byte, 4096>{};
    auto writer = nxt::rt::byte_writer{out, std::span{storage}};
    co_await print_entries(writer, entries);
}

} // namespace

int main(int argc, char ** argv)
try {
    auto path = argc > 1
        ? std::string{argv[1]}
        : std::string{"."};

    auto wand = nxt::rt::uring_wand{};
    auto deck = nxt::rt::deck{&wand};
    auto task = list_and_print(path);

    deck.start(task);
    pump_until_done(deck, wand, task);

    std::move(task).result();
    return 0;
} catch (std::exception const & error) {
    std::cerr << "ng-uring-wand-demo: " << error.what() << '\n';
    return 1;
}
