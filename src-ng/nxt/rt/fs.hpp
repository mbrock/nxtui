#pragma once

#include "nxt/rt/buffers.hpp"
#include "nxt/rt/task.hpp"
#include <nxt/unique-fd.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/vnode.h>
#endif

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

inline file_status status_from_stat(struct stat const & stat) noexcept
{
    return file_status{
        .kind = kind_from_mode(stat.st_mode),
        .size = static_cast<std::uint64_t>(stat.st_size),
        .mode = stat.st_mode,
    };
}

inline std::runtime_error syscall_error(std::string_view operation)
{
    return std::runtime_error{
        std::string{operation} + " failed: " + std::strerror(errno)};
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

#elif defined(__APPLE__)

namespace detail {

inline file_kind kind_from_vnode_type(fsobj_type_t type) noexcept
{
    switch (type) {
    case VREG:
        return file_kind::regular;
    case VDIR:
        return file_kind::directory;
    case VLNK:
        return file_kind::symlink;
    case VCHR:
        return file_kind::character;
    case VBLK:
        return file_kind::block;
    case VFIFO:
        return file_kind::fifo;
    case VSOCK:
        return file_kind::socket;
    default:
        return file_kind::other;
    }
}

template<typename T>
T read_packed(char const *& field, char const * end)
{
    if (field + sizeof(T) > end)
        throw std::runtime_error{"getattrlistbulk returned a short record"};

    auto value = T{};
    std::memcpy(&value, field, sizeof(T));
    field += sizeof(T);
    return value;
}

inline std::string read_attr_name(
    char const * ref_field,
    attrreference_t const & reference,
    char const * record_end)
{
    auto const * name = ref_field + reference.attr_dataoffset;
    auto const * name_end = name + reference.attr_length;
    if (name < ref_field || name_end > record_end || reference.attr_length == 0)
        throw std::runtime_error{"getattrlistbulk returned a bad name"};

    auto size = static_cast<std::size_t>(reference.attr_length);
    if (name[size - 1] == '\0')
        --size;
    return std::string{name, size};
}

inline task<file_status> stat_path(std::string path)
{
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0)
        throw syscall_error("lstat");
    co_return status_from_stat(info);
}

inline file_status stat_at(int dirfd, char const * name)
{
    struct stat info {};
    if (::fstatat(dirfd, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        throw syscall_error("fstatat");
    return status_from_stat(info);
}

inline mode_t type_bits(file_kind kind) noexcept
{
    switch (kind) {
    case file_kind::regular:
        return S_IFREG;
    case file_kind::directory:
        return S_IFDIR;
    case file_kind::symlink:
        return S_IFLNK;
    case file_kind::character:
        return S_IFCHR;
    case file_kind::block:
        return S_IFBLK;
    case file_kind::fifo:
        return S_IFIFO;
    case file_kind::socket:
        return S_IFSOCK;
    default:
        return 0;
    }
}

inline void append_bulk_entries(
    std::vector<directory_entry> & entries,
    std::span<std::byte const> bytes,
    int count)
{
    auto const * entry =
        reinterpret_cast<char const *>(bytes.data());
    auto const * const end = entry + bytes.size();

    for (auto i = 0; i != count; ++i) {
        auto const * const record = entry;
        auto length = read_packed<std::uint32_t>(entry, end);
        if (length < sizeof(std::uint32_t))
            throw std::runtime_error{
                "getattrlistbulk returned a short record"};

        auto const * const record_end = record + length;
        if (record_end > end)
            throw std::runtime_error{
                "getattrlistbulk returned a record past the buffer"};

        auto returned = read_packed<attribute_set_t>(entry, record_end);
        auto entry_error = std::uint32_t{};
        if ((returned.commonattr & ATTR_CMN_ERROR) != 0)
            entry_error = read_packed<std::uint32_t>(entry, record_end);

        auto name = std::string{};
        if ((returned.commonattr & ATTR_CMN_NAME) != 0) {
            auto const * ref_field = entry;
            auto reference = read_packed<attrreference_t>(entry, record_end);
            name = read_attr_name(ref_field, reference, record_end);
        }

        if (entry_error != 0) {
            entry = record_end;
            continue;
        }

        auto status = file_status{};
        if ((returned.commonattr & ATTR_CMN_OBJTYPE) != 0)
            status.kind =
                kind_from_vnode_type(read_packed<fsobj_type_t>(entry, record_end));
        if ((returned.commonattr & ATTR_CMN_ACCESSMASK) != 0)
            status.mode =
                read_packed<std::uint32_t>(entry, record_end)
                | type_bits(status.kind);
        else
            status.mode = type_bits(status.kind);
        if ((returned.dirattr & ATTR_DIR_DATALENGTH) != 0)
            status.size = read_packed<std::uint64_t>(entry, record_end);
        if ((returned.fileattr & ATTR_FILE_TOTALSIZE) != 0)
            status.size = read_packed<std::uint64_t>(entry, record_end);

        entries.push_back(directory_entry{
            .name = std::move(name),
            .status = status,
        });
        entry = record_end;
    }
}

} // namespace detail

inline task<std::vector<directory_entry>> list_directory(std::string path)
{
    auto fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        throw detail::syscall_error("open");
    auto dir = nxt::unique_fd{fd};

    auto attrs = attrlist{
        .bitmapcount = ATTR_BIT_MAP_COUNT,
        .reserved = 0,
        .commonattr = ATTR_CMN_RETURNED_ATTRS
            | ATTR_CMN_NAME
            | ATTR_CMN_ERROR
            | ATTR_CMN_OBJTYPE
            | ATTR_CMN_ACCESSMASK,
        .volattr = 0,
        .dirattr = ATTR_DIR_DATALENGTH,
        .fileattr = ATTR_FILE_TOTALSIZE,
        .forkattr = 0,
    };
    auto buffer = std::array<std::byte, 16 * 1024>{};

    auto entries = std::vector<directory_entry>{};
    entries.push_back(directory_entry{
        .name = ".",
        .status = detail::stat_at(dir.get(), "."),
    });
    entries.push_back(directory_entry{
        .name = "..",
        .status = detail::stat_at(dir.get(), ".."),
    });

    while (true) {
        auto count = ::getattrlistbulk(
            dir.get(),
            &attrs,
            buffer.data(),
            buffer.size(),
            FSOPT_NOFOLLOW | FSOPT_REPORT_FULLSIZE);
        if (count < 0)
            throw detail::syscall_error("getattrlistbulk");
        if (count == 0)
            break;

        detail::append_bulk_entries(entries, std::span{buffer}, count);
    }

    std::ranges::sort(entries, {}, &directory_entry::name);
    co_return entries;
}

inline task<std::vector<directory_entry>> list_path(std::string path)
{
    auto status = co_await detail::stat_path(path);

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
