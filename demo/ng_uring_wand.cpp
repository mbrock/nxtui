#include <nxt/rt/buffers.hpp>
#include <nxt/rt/uring_wand.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

struct [[gnu::packed]] linux_dirent64_header
{
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
};

struct listing_entry
{
    std::string name;
    struct statx stat{};
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
    auto source = nxt::rt::task_byte_source{
        [fd = dir.get()](std::span<std::byte> dst)
            -> nxt::rt::task<nxt::rt::read_result> {
            auto n = co_await nxt::rt::getdents64_wish{
                .fd = fd,
                .buffer = dst,
            };
            co_return nxt::rt::read_result{
                .bytes = n,
                .eof = n == 0,
            };
        }};
    auto storage = std::array<std::byte, 16 * 1024>{};
    auto reader = nxt::rt::byte_reader{source, std::span{storage}};

    auto entries = std::vector<listing_entry>{};
    while (true) {
        auto header = linux_dirent64_header{};
        try {
            header = co_await reader.take_struct<linux_dirent64_header>();
        } catch (const nxt::rt::end_of_stream &) {
            break;
        }

        if (header.d_reclen < sizeof(linux_dirent64_header))
            throw std::runtime_error{"getdents64 returned a short entry"};

        auto name = co_await reader.take_string_view(
            header.d_reclen - sizeof(linux_dirent64_header));
        name = name.substr(0, name.find('\0'));
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

nxt::rt::task<void> list_and_print(std::string path)
{
    auto out = nxt::rt::fd_sink{STDOUT_FILENO};
    auto storage = std::array<std::byte, 4096>{};
    auto writer = nxt::rt::byte_writer{out, std::span{storage}};

    auto entries = co_await list_path(std::move(path));
    auto tasks = entries
        | std::views::transform([](listing_entry const & entry) {
            return std::format(
                "{} {:>8} {}\n",
                mode_string(entry.stat.stx_mode),
                entry.stat.stx_size,
                entry.name);
        })
        | std::views::transform([&](std::string line) {
            return writer.write(std::move(line));
        });

    co_await nxt::rt::for_each_task(tasks);
    co_await writer.flush();
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
    wand.run_until_done(deck, task);

    std::move(task).result();
    return 0;
} catch (std::exception const & error) {
    std::cerr << "ng-uring-wand-demo: " << error.what() << '\n';
    return 1;
}
