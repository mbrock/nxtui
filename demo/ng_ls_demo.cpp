#include "nxt/rt/task.hpp"
#include <nxt/rt/buffers.hpp>
#include <nxt/rt/fs.hpp>

#if defined(__linux__)
#include <nxt/rt/uring_wand.hpp>
#else
#include <nxt/rt/kqueue_wand.hpp>
#endif

#include <array>
#include <format>
#include <iostream>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace {

char kind_char(nxt::rt::fs::file_kind kind)
{
    using enum nxt::rt::fs::file_kind;
    switch (kind) {
    case directory:
        return 'd';
    case symlink:
        return 'l';
    case character:
        return 'c';
    case block:
        return 'b';
    case fifo:
        return 'p';
    case socket:
        return 's';
    default:
        return '-';
    }
}

std::string mode_string(nxt::rt::fs::file_status const & status)
{
    auto result = std::string{"----------"};
    result[0] = kind_char(status.kind);

    constexpr auto bits = std::array{
        S_IRUSR,
        S_IWUSR,
        S_IXUSR,
        S_IRGRP,
        S_IWGRP,
        S_IXGRP,
        S_IROTH,
        S_IWOTH,
        S_IXOTH,
    };
    constexpr auto chars = std::array{
        'r',
        'w',
        'x',
        'r',
        'w',
        'x',
        'r',
        'w',
        'x',
    };

    for (auto i = std::size_t{}; i != bits.size(); ++i) {
        if ((status.mode & bits[i]) != 0)
            result[i + 1] = chars[i];
    }

    return result;
}

} // namespace

int main(int argc, char ** argv)
try {
    auto path = argc > 1 ? std::string{argv[1]} : std::string{"."};

    auto body = [path = std::move(path)]() mutable -> nxt::rt::task<void> {
        co_await nxt::rt::write_all(
            nxt::rt::standard_output(),
            (co_await nxt::rt::fs::list_path(std::move(path)))
                | std::views::transform(
                    [](nxt::rt::fs::directory_entry const & entry) {
                        return std::format(
                            "{} {:>8} {}\n",
                            mode_string(entry.status),
                            entry.status.size,
                            entry.name);
                    }));
    };

#if defined(__linux__)
    nxt::rt::run(std::move(body));
#elif NXT_RT_HAS_KQUEUE
    nxt::rt::run_with_kqueue(std::move(body));
#else
    static_assert(NXT_RT_HAS_KQUEUE, "ng ls demo needs a runtime wand");
#endif

    return 0;
} catch (std::exception const & error) {
    std::cerr << "ng-ls-demo: " << error.what() << '\n';
    return 1;
}
