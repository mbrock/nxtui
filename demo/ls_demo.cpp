#include "nxtrt/task.hpp"
#include <nxtrt/buffers.hpp>
#include <nxtrt/fs.hpp>

#if defined(__linux__)
#include <nxtrt/wand/uring.hpp>
#else
#include <nxtrt/wand/kqueue.hpp>
#endif

#include <array>
#include <format>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace {

char kind_char(nxtrt::fs::file_kind kind)
{
    using enum nxtrt::fs::file_kind;
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

std::string mode_string(nxtrt::fs::file_status const & status)
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

nxtrt::task<void> list_path_to_stdout(std::string path)
{
    auto out = nxtrt::standard_output_sink();
    for (auto const & entry : co_await nxtrt::fs::list_path(std::move(path)))
        co_await nxtrt::write(
            out,
            std::format(
                "{} {:>8} {}\n",
                mode_string(entry.status),
                entry.status.size,
                entry.name));
    co_await out.flush();
}

} // namespace

int nxt_ls_demo_main(int argc, char ** argv)
try {
    auto path = argc > 1 ? std::string{argv[1]} : std::string{"."};

#if defined(__linux__)
    nxtrt::run(list_path_to_stdout(std::move(path)));
#elif NXT_RT_HAS_KQUEUE
    nxtrt::run_with_kqueue(list_path_to_stdout(std::move(path)));
#else
    static_assert(NXT_RT_HAS_KQUEUE, "ls demo needs a runtime wand");
#endif

    return 0;
} catch (std::exception const & error) {
    std::cerr << "nxt-ls-demo: " << error.what() << '\n';
    return 1;
}

#if !defined(NXT_EMBEDDED_MAIN)
int main(int argc, char ** argv)
{
    return nxt_ls_demo_main(argc, argv);
}
#endif
