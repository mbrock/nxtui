// nxtdemo — entry point for the bundled UI demos. Pick one via the
// first argument; remaining arguments are forwarded to the demo.

#include <cstdio>
#include <cstring>

namespace nxt::build_sim {
int run(int argc, char ** argv);
}
namespace nxt::cassette {
int run(int argc, char ** argv);
}
namespace nxt::cgroup_browser {
int run(int argc, char ** argv);
}
#ifdef NXTDEMO_HAVE_SPAN_BROWSER
namespace nxt::span_browser {
int run(int argc, char ** argv);
}
#endif

namespace {

void print_usage(const char * program)
{
    std::fprintf(
        stderr,
        "usage: %s <demo> [args...]\n"
        "demos:\n"
        "  build_sim       fake parallel build progress\n"
        "  cassette        render an agent trace as cassette cards\n"
        "  cgroup_browser  walk /sys/fs/cgroup\n"
#ifdef NXTDEMO_HAVE_SPAN_BROWSER
        "  span_browser    browse an otel parquet archive\n"
#endif
        ,
        program);
}

} // namespace

int main(int argc, char ** argv)
{
    const auto * program = argc > 0 ? argv[0] : "nxtdemo";

    if (argc < 2) {
        print_usage(program);
        return 2;
    }

    const auto * name = argv[1];
    auto sub_argc = argc - 1;
    auto sub_argv = argv + 1;

    if (std::strcmp(name, "build_sim") == 0)
        return nxt::build_sim::run(sub_argc, sub_argv);
    if (std::strcmp(name, "cassette") == 0)
        return nxt::cassette::run(sub_argc, sub_argv);
    if (std::strcmp(name, "cgroup_browser") == 0)
        return nxt::cgroup_browser::run(sub_argc, sub_argv);
#ifdef NXTDEMO_HAVE_SPAN_BROWSER
    if (std::strcmp(name, "span_browser") == 0)
        return nxt::span_browser::run(sub_argc, sub_argv);
#endif

    std::fprintf(stderr, "%s: unknown demo '%s'\n", program, name);
    print_usage(program);
    return 2;
}
