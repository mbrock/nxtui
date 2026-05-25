#pragma once

#include "nxt/rt/buffers.hpp"
#include "nxt/rt/task.hpp"

#include <nxt/unique-fd.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <system_error>
#include <unordered_map>

namespace nxt::rt::cgroup {

namespace detail {

struct [[gnu::packed]] linux_dirent64_header
{
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
};

inline task<std::vector<std::string>> list_names(std::filesystem::path path)
{
    auto fd = co_await op::openat{
        .dirfd = AT_FDCWD,
        .path = path.string(),
        .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        .mode = 0,
    };
    auto dir = nxt::unique_fd{fd};
    auto storage = std::array<std::byte, 16 * 1024>{};
    auto names = std::vector<std::string>{};

    while (true) {
        auto bytes = co_await op::getdents64{
            .fd = dir.get(),
            .buffer = std::span{storage},
        };
        if (bytes == 0)
            break;

        for (auto offset = std::size_t{}; offset < bytes;) {
            auto const * entry =
                reinterpret_cast<linux_dirent64_header const *>(
                    storage.data() + offset);
            if (entry->d_reclen < sizeof(linux_dirent64_header))
                throw runtime_error{"getdents64 returned a short entry"};

            auto name = std::string{
                reinterpret_cast<const char *>(
                    storage.data() + offset + sizeof(linux_dirent64_header))};
            if (name != "." && name != "..")
                names.push_back(std::move(name));
            offset += entry->d_reclen;
        }
    }

    co_return names;
}

} // namespace detail

struct bytes_t
{
    std::uint64_t v = 0;
};

struct count_t
{
    std::uint64_t v = 0;
};

struct usec_t
{
    std::uint64_t v = 0;
};

struct sample
{
    std::chrono::steady_clock::time_point at;
    bytes_t memory_current;
    bytes_t memory_peak;
    count_t pids;
    usec_t cpu_usage;
    usec_t cpu_user;
    usec_t cpu_system;
    double psi_mem = 0.0;
    double psi_cpu = 0.0;
    double psi_io = 0.0;
};

inline task<std::string> read_text_file(std::filesystem::path path)
{
    try {
        auto fd = co_await op::openat{
            .dirfd = AT_FDCWD,
            .path = path.string(),
            .flags = O_RDONLY | O_CLOEXEC,
            .mode = 0,
        };
        auto file = nxt::unique_fd{fd};
        auto storage = std::array<std::byte, 4096>{};
        auto out = std::string{};

        while (true) {
            auto n = co_await op::read_some{
                .fd = file.get(),
                .buffer = std::span{storage},
            };
            if (n == 0)
                break;
            out += as_string_view(std::span{storage}.first(n));
        }
        co_return out;
    } catch (const runtime_error &) {
        co_return std::string{};
    }
}

inline task<std::uint64_t> read_uint_file(std::filesystem::path path)
{
    auto text = co_await read_text_file(std::move(path));
    auto value = std::uint64_t{};
    auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{})
        co_return 0;
    co_return value;
}

inline std::unordered_map<std::string, std::uint64_t>
parse_kv(std::string_view text)
{
    auto out = std::unordered_map<std::string, std::uint64_t>{};
    auto p = text.data();
    auto end = p + text.size();
    while (p < end) {
        auto le = std::find(p, end, '\n');
        auto sp = std::find(p, le, ' ');
        if (sp != le && sp != p) {
            auto key = std::string{p, sp};
            auto value = std::uint64_t{};
            for (char ch : std::string_view{sp + 1, le}) {
                if (ch < '0' || ch > '9')
                    break;
                value = value * 10 + static_cast<std::uint64_t>(ch - '0');
            }
            out.emplace(std::move(key), value);
        }
        p = le + 1;
    }
    return out;
}

inline double parse_psi_some_avg10(std::string_view text)
{
    auto needle = std::string_view{"some avg10="};
    auto pos = text.find(needle);
    if (pos == std::string_view::npos)
        return 0.0;
    pos += needle.size();
    auto end = text.find(' ', pos);
    if (end == std::string_view::npos)
        end = text.size();
    try {
        return std::stod(std::string{text.substr(pos, end - pos)});
    } catch (...) {
        return 0.0;
    }
}

inline task<sample> read_sample(const std::filesystem::path & dir)
{
    auto out = sample{};

    auto [
        memory_current,
        memory_peak,
        pids,
        cpu_text,
        mem_pressure,
        cpu_pressure,
        io_pressure] = co_await when_all(
        read_uint_file(dir / "memory.current"),
        read_uint_file(dir / "memory.peak"),
        read_uint_file(dir / "pids.current"),
        read_text_file(dir / "cpu.stat"),
        read_text_file(dir / "memory.pressure"),
        read_text_file(dir / "cpu.pressure"),
        read_text_file(dir / "io.pressure"));

    out.at = std::chrono::steady_clock::now();
    out.memory_current = bytes_t{memory_current};
    out.memory_peak = bytes_t{memory_peak};
    out.pids = count_t{pids};

    auto cpu = parse_kv(cpu_text);
    auto lookup = [&](std::string_view key) {
        auto it = cpu.find(std::string{key});
        return it == cpu.end() ? std::uint64_t{} : it->second;
    };
    out.cpu_usage = usec_t{lookup("usage_usec")};
    out.cpu_user = usec_t{lookup("user_usec")};
    out.cpu_system = usec_t{lookup("system_usec")};
    out.psi_mem = parse_psi_some_avg10(mem_pressure);
    out.psi_cpu = parse_psi_some_avg10(cpu_pressure);
    out.psi_io = parse_psi_some_avg10(io_pressure);
    co_return out;
}

inline task<std::optional<std::filesystem::path>> find_unit_scope(
    std::string unit_name,
    std::filesystem::path root = "/sys/fs/cgroup")
{
    auto needle = unit_name + ".scope";
    auto names = std::vector<std::string>{};
    try {
        names = co_await detail::list_names(root);
    } catch (const runtime_error &) {
        co_return std::nullopt;
    }

    for (auto const & name : names) {
        auto path = root / name;
        if (name == needle)
            co_return path;

        if (auto found = co_await find_unit_scope(unit_name, std::move(path)))
            co_return found;
    }

    co_return std::nullopt;
}

} // namespace nxt::rt::cgroup
