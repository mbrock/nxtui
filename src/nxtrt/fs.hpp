#pragma once

#include "nxtrt/blocking.hpp"
#include "nxtrt/buffers.hpp"
#include "nxtrt/task.hpp"
#include <nxt/unique-fd.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/vnode.h>
#endif

namespace nxtrt::fs {

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
    std::int64_t modified_ms = 0; // Unix epoch milliseconds
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
#if defined(__APPLE__)
    auto const & modified = stat.st_mtimespec;
#else
    auto const & modified = stat.st_mtim;
#endif
    return file_status{
        .kind = kind_from_mode(stat.st_mode),
        .size = static_cast<std::uint64_t>(stat.st_size),
        .mode = stat.st_mode,
        .modified_ms = std::int64_t(modified.tv_sec) * 1000
            + modified.tv_nsec / 1000000,
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
        .modified_ms = std::int64_t(stat.stx_mtime.tv_sec) * 1000
            + stat.stx_mtime.tv_nsec / 1000000,
    };
}

inline task<file_status> stat_path(int dirfd, std::string path)
{
    auto stat = co_await op::statx{
        dirfd,
        std::move(path),
        AT_SYMLINK_NOFOLLOW,
        STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_MTIME};
    co_return status_from_statx(stat);
}

inline task<std::vector<std::string>> read_directory_names(int fd)
{
    auto storage = std::array<std::byte, 16 * 1024>{};
    auto source = task_bytefeed{
        [fd](junk<std::byte> dst) -> task<std::size_t> {
            co_return co_await op::getdents64{fd, dst.as_writable_bytes()};
        },
        std::span{storage}};

    auto names = std::vector<std::string>{};
    while (
        auto header = co_await source.take_struct<linux_dirent64_header>()) {
        if (header->d_reclen < sizeof(linux_dirent64_header))
            throw std::runtime_error{"getdents64 returned a short entry"};

        auto name = co_await source.take_string_view(
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
        AT_FDCWD,
        std::move(path),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC};
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
        if ((returned.commonattr & ATTR_CMN_MODTIME) != 0) {
            auto modified = read_packed<timespec>(entry, record_end);
            status.modified_ms = std::int64_t(modified.tv_sec) * 1000
                + modified.tv_nsec / 1000000;
        }
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

// Every entry of an open directory except "." and "..", with names,
// kinds, sizes and times from one getattrlistbulk call per batch.
inline void append_bulk_directory(
    std::vector<directory_entry> & entries,
    int dir)
{
    auto attrs = attrlist{
        .bitmapcount = ATTR_BIT_MAP_COUNT,
        .reserved = 0,
        .commonattr = ATTR_CMN_RETURNED_ATTRS
            | ATTR_CMN_NAME
            | ATTR_CMN_ERROR
            | ATTR_CMN_OBJTYPE
            | ATTR_CMN_MODTIME
            | ATTR_CMN_ACCESSMASK,
        .volattr = 0,
        .dirattr = ATTR_DIR_DATALENGTH,
        .fileattr = ATTR_FILE_TOTALSIZE,
        .forkattr = 0,
    };
    auto buffer = std::array<std::byte, 16 * 1024>{};
    while (true) {
        auto count = ::getattrlistbulk(
            dir,
            &attrs,
            buffer.data(),
            buffer.size(),
            FSOPT_NOFOLLOW | FSOPT_REPORT_FULLSIZE);
        if (count < 0)
            throw syscall_error("getattrlistbulk");
        if (count == 0)
            break;
        append_bulk_entries(entries, std::span{buffer}, count);
    }
}

} // namespace detail

inline task<std::vector<directory_entry>> list_directory(std::string path)
{
    auto fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        throw detail::syscall_error("open");
    auto dir = nxt::unique_fd{fd};

    auto entries = std::vector<directory_entry>{};
    entries.push_back(directory_entry{
        .name = ".",
        .status = detail::stat_at(dir.get(), "."),
    });
    entries.push_back(directory_entry{
        .name = "..",
        .status = detail::stat_at(dir.get(), ".."),
    });
    detail::append_bulk_directory(entries, dir.get());

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

// --- Paths confined beneath a directory ------------------------------------
//
// Like WASI preopens: a granted directory descriptor is the capability, and
// a relative path can only name things beneath it. Every segment must be a
// plain name (no empty, ".", ".." or NUL), and no symlink is ever followed,
// including in the final segment, so resolution cannot leave the directory
// even when the tree contains links. That is stricter than WASI, which
// follows links that stay beneath. An empty path names the directory.
//
// These are direct, synchronous *at syscalls; `files` below runs them off
// the deck or replaces them with io_uring operations. Failures are
// errno_error for the OS and std::invalid_argument for a malformed path.

inline std::vector<std::string_view> beneath_segments(std::string_view path)
{
    auto segments = std::vector<std::string_view>{};
    if (path.empty())
        return segments;
    for (auto part : std::views::split(path, '/')) {
        auto name = std::string_view{part.begin(), part.end()};
        if (name.empty() || name == "." || name == ".."
            || name.find('\0') != name.npos)
            throw std::invalid_argument{
                "path must be relative, '/'-separated plain names"};
        segments.push_back(name);
    }
    return segments;
}

struct beneath_entry
{
    nxt::unique_fd parent;
    std::string name; // empty: PATH named the directory itself
};

namespace detail {

[[noreturn]] inline void throw_errno(char const * operation)
{
    auto code = errno;
    throw errno_error{
        code, std::string{operation} + " failed: " + std::strerror(code)};
}

} // namespace detail

inline beneath_entry resolve_beneath(int dirfd, std::string_view path)
{
    auto segments = beneath_segments(path);
    auto parent = nxt::unique_fd{::fcntl(dirfd, F_DUPFD_CLOEXEC, 0)};
    if (parent.get() < 0)
        detail::throw_errno("dup");
    if (segments.empty())
        return {std::move(parent), {}};
    for (auto name : segments | std::views::take(segments.size() - 1)) {
        auto next = ::openat(
            parent.get(),
            std::string{name}.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            // Report a refused symlink as ELOOP everywhere; macOS says
            // ENOTDIR when O_DIRECTORY meets one.
            auto error = errno;
            struct stat info {};
            if (error == ENOTDIR
                && ::fstatat(
                       parent.get(),
                       std::string{name}.c_str(),
                       &info,
                       AT_SYMLINK_NOFOLLOW)
                       == 0
                && S_ISLNK(info.st_mode))
                error = ELOOP;
            errno = error;
            detail::throw_errno("openat");
        }
        parent.reset(next);
    }
    return {std::move(parent), std::string{segments.back()}};
}

/// Status of PATH beneath DIRFD; a final symlink reports itself.
inline struct stat stat_beneath(int dirfd, std::string_view path)
{
    auto entry = resolve_beneath(dirfd, path);
    struct stat info {};
    auto rc = entry.name.empty()
        ? ::fstat(entry.parent.get(), &info)
        : ::fstatat(
              entry.parent.get(),
              entry.name.c_str(),
              &info,
              AT_SYMLINK_NOFOLLOW);
    if (rc != 0)
        detail::throw_errno("fstatat");
    return info;
}

/// Opens PATH beneath DIRFD with FLAGS plus O_NOFOLLOW, so a final symlink
/// fails with ELOOP. Pass O_NONBLOCK so a FIFO cannot stall the open. An
/// empty PATH returns a duplicate of DIRFD and ignores FLAGS.
inline nxt::unique_fd
open_beneath(int dirfd, std::string_view path, int flags = O_RDONLY)
{
    auto entry = resolve_beneath(dirfd, path);
    if (entry.name.empty())
        return std::move(entry.parent);
    auto fd = ::openat(
        entry.parent.get(),
        entry.name.c_str(),
        flags | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        detail::throw_errno("openat");
    return nxt::unique_fd{fd};
}

/// Entries of an open directory sorted by name, without "." and "..",
/// statted without following symlinks. Synchronous: macOS reads names and
/// attributes in getattrlistbulk batches, elsewhere readdir plus fstatat.
inline std::vector<directory_entry> list_entries(nxt::unique_fd dir)
{
    auto entries = std::vector<directory_entry>{};
#if defined(__APPLE__)
    detail::append_bulk_directory(entries, dir.get());
#else
    auto * stream = ::fdopendir(dir.get());
    if (!stream)
        detail::throw_errno("fdopendir");
    (void) dir.release(); // closedir owns it now
    struct closer
    {
        DIR * stream;
        ~closer()
        {
            ::closedir(stream);
        }
    } guard{stream};
    errno = 0;
    while (auto * entry = ::readdir(stream)) {
        auto name = std::string_view{entry->d_name};
        if (name != "." && name != "..") {
            struct stat info {};
            if (::fstatat(::dirfd(stream), entry->d_name, &info,
                          AT_SYMLINK_NOFOLLOW) != 0)
                detail::throw_errno("fstatat");
            entries.push_back(directory_entry{
                .name = std::string{name},
                .status = detail::status_from_stat(info),
            });
        }
        errno = 0;
    }
    if (errno)
        detail::throw_errno("readdir");
#endif
    std::ranges::sort(entries, {}, &directory_entry::name);
    return entries;
}

/// Filesystem calls that keep the deck responsive. On a wand whose
/// asynchronous_files() holds (io_uring), opens and stats are kernel
/// operations, and openat2 with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS
/// enforces the same confinement in the kernel. Otherwise the synchronous
/// functions above run on WORKERS, as does listing everywhere (io_uring has
/// no getdents). Failures are errno_error; malformed paths are rejected on
/// the deck with std::invalid_argument. DIRFD is borrowed until the call
/// settles, and the files object must outlive its calls. One difference
/// remains: io_uring opens a FIFO without waiting for its peer, as if
/// O_NONBLOCK were given, where the pool's open(2) waits.
class files
{
public:
    explicit files(blocking_pool & workers)
        : workers_(workers)
    {}

    task<nxt::unique_fd>
    open_beneath(int dirfd, std::string path, int flags = O_RDONLY);
    task<file_status> stat_beneath(int dirfd, std::string path);
    task<std::vector<directory_entry>>
    list_beneath(int dirfd, std::string path);
    /// pread into BUFFER, which must stay alive until the call settles.
    task<std::size_t>
    read_at(int fd, std::span<std::byte> buffer, std::uint64_t offset);

private:
    static bool asynchronous() noexcept
    {
        auto * d = current_deck();
        return d && d->current_wand() && d->current_wand()->asynchronous_files();
    }

    blocking_pool & workers_;
};

#if defined(__linux__)
namespace detail {

inline constexpr std::uint64_t confined =
    RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

} // namespace detail
#endif

inline task<nxt::unique_fd>
files::open_beneath(int dirfd, std::string path, int flags)
{
    (void) beneath_segments(path);
#if defined(__linux__)
    if (asynchronous() && !path.empty()) {
        auto fd = co_await op::openat2{
            dirfd,
            std::move(path),
            std::uint64_t(flags | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC),
            detail::confined};
        co_return nxt::unique_fd{fd};
    }
#endif
    co_return co_await workers_.run(
        [dirfd, path = std::move(path), flags] {
            return fs::open_beneath(dirfd, path, flags);
        });
}

inline task<file_status> files::stat_beneath(int dirfd, std::string path)
{
    (void) beneath_segments(path);
#if defined(__linux__)
    if (asynchronous()) {
        // An O_PATH | O_NOFOLLOW open yields the final symlink itself even
        // under RESOLVE_NO_SYMLINKS, so it can be reported, not followed.
        auto opened = nxt::unique_fd{};
        if (!path.empty())
            opened = nxt::unique_fd{co_await op::openat2{
                dirfd,
                std::move(path),
                std::uint64_t(O_PATH | O_NOFOLLOW | O_CLOEXEC),
                detail::confined}};
        auto info = co_await op::statx{
            opened.get() < 0 ? dirfd : opened.get(),
            "",
            AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
            STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_MTIME};
        co_return detail::status_from_statx(info);
    }
#endif
    co_return co_await workers_.run([dirfd, path = std::move(path)] {
        return detail::status_from_stat(fs::stat_beneath(dirfd, path));
    });
}

inline task<std::size_t>
files::read_at(int fd, std::span<std::byte> buffer, std::uint64_t offset)
{
    if (asynchronous())
        co_return co_await op::read_some{fd, buffer, off_t(offset)};
    co_return co_await workers_.run([fd, buffer, offset] {
        while (true) {
            auto count = ::pread(fd, buffer.data(), buffer.size(), off_t(offset));
            if (count >= 0)
                return std::size_t(count);
            if (errno != EINTR)
                detail::throw_errno("pread");
        }
    });
}

inline task<std::vector<directory_entry>>
files::list_beneath(int dirfd, std::string path)
{
    (void) beneath_segments(path);
    co_return co_await workers_.run([dirfd, path = std::move(path)] {
        return list_entries(
            fs::open_beneath(dirfd, path, O_RDONLY | O_DIRECTORY | O_NONBLOCK));
    });
}

} // namespace nxtrt::fs
