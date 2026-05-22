#pragma once

#include "nxt/rt/buffers.hpp"
#include "nxt/rt/task.hpp"
#include <nxt/unique-fd.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace nxt::rt::fs {

enum class file_kind
{
    regular,
    directory,
    symlink,
    character,
    block,
    fifo,
    socket,
    other,
};

struct file_status
{
    file_kind kind = file_kind::other;
    std::uint64_t size = 0;
    mode_t mode = 0;
};

struct directory_entry
{
    std::string name;
    file_status status{};
};

namespace detail {

inline file_kind kind_from_mode(mode_t mode) noexcept
{
    if (S_ISREG(mode))
        return file_kind::regular;
    if (S_ISDIR(mode))
        return file_kind::directory;
    if (S_ISLNK(mode))
        return file_kind::symlink;
    if (S_ISCHR(mode))
        return file_kind::character;
    if (S_ISBLK(mode))
        return file_kind::block;
    if (S_ISFIFO(mode))
        return file_kind::fifo;
    if (S_ISSOCK(mode))
        return file_kind::socket;
    return file_kind::other;
}

} // namespace detail

#if defined(__linux__)

namespace detail {

struct [[gnu::packed]] linux_dirent64_header
{
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
};

inline file_status status_from_statx(statx_result const & stat) noexcept
{
    return file_status{
        .kind = kind_from_mode(stat.stx_mode),
        .size = stat.stx_size,
        .mode = stat.stx_mode,
    };
}

inline task<file_status> stat_path(int dirfd, std::string path)
{
    auto stat = co_await op::statx{
        .dirfd = dirfd,
        .path = std::move(path),
        .flags = AT_SYMLINK_NOFOLLOW,
        .mask = STATX_TYPE | STATX_MODE | STATX_SIZE,
    };
    co_return status_from_statx(stat);
}

inline task<std::vector<std::string>> read_directory_names(int fd)
{
    auto source = task_byte_source{
        [fd](std::span<std::byte> dst) -> task<std::size_t> {
            co_return co_await op::getdents64{
                .fd = fd,
                .buffer = dst,
            };
        }};
    auto storage = std::array<std::byte, 16 * 1024>{};
    auto reader = byte_reader{source, std::span{storage}};

    auto names = std::vector<std::string>{};
    while (auto header = co_await reader.take_struct<linux_dirent64_header>()) {
        if (header->d_reclen < sizeof(linux_dirent64_header))
            throw std::runtime_error{"getdents64 returned a short entry"};

        auto name = co_await reader.take_string_view(
            header->d_reclen - sizeof(linux_dirent64_header));
        name = name.substr(0, name.find('\0'));
        names.emplace_back(name);
    }

    co_return names;
}

} // namespace detail

inline task<std::vector<directory_entry>> list_directory(std::string path)
{
    auto fd = co_await op::openat{
        .dirfd = AT_FDCWD,
        .path = std::move(path),
        .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        .mode = 0,
    };
    auto dir = nxt::unique_fd{fd};
    auto names = co_await detail::read_directory_names(dir.get());

    auto entries = co_await when_all_range(
        names
        | std::views::transform(
            [dirfd = dir.get()](
                std::string const & name) -> task<directory_entry> {
                co_return directory_entry{
                    .name = name,
                    .status = co_await detail::stat_path(dirfd, name),
                };
            }));

    std::ranges::sort(entries, {}, &directory_entry::name);
    co_return entries;
}

inline task<std::vector<directory_entry>> list_path(std::string path)
{
    auto status = co_await detail::stat_path(AT_FDCWD, path);

    if (status.kind == file_kind::directory)
        co_return co_await list_directory(std::move(path));

    co_return std::vector<directory_entry>{
        directory_entry{
            .name = std::move(path),
            .status = status,
        },
    };
}

#endif

} // namespace nxt::rt::fs
