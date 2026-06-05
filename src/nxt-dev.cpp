#include <cstring>
#include <iostream>
#include <string_view>

int nxt_tests_main(int argc, char ** argv);
int nxtllm_main(int argc, char ** argv);
int nxt_ls_demo_main(int argc, char ** argv);
int nxt_tui_demo_main(int argc, char ** argv);
#if defined(__linux__)
int nxt_shell_scope_demo_main(int argc, char ** argv);
#endif
int nxt_http_client_demo_main(int argc, char ** argv);
int nxt_openai_sse_demo_main(int argc, char ** argv);

namespace {

struct command
{
    std::string_view name;
    int (*run)(int, char **);
};

constexpr command commands[] = {
    {"test", nxt_tests_main},
    {"nxtllm", nxtllm_main},
    {"ls", nxt_ls_demo_main},
    {"tui", nxt_tui_demo_main},
#if defined(__linux__)
    {"shell-scope", nxt_shell_scope_demo_main},
#endif
    {"http-client", nxt_http_client_demo_main},
    {"openai-sse", nxt_openai_sse_demo_main},
};

void print_usage(const char * program)
{
    std::cerr << "usage: " << program << " <command> [args...]\n"
              << "commands:\n";
    for (auto const & command : commands)
        std::cerr << "  " << command.name << '\n';
}

} // namespace

int main(int argc, char ** argv)
{
    auto const * program = argc > 0 ? argv[0] : "nxt-dev";
    if (argc < 2) {
        print_usage(program);
        return 2;
    }

    auto const name = std::string_view{argv[1]};
    for (auto const & command : commands) {
        if (name == command.name)
            return command.run(argc - 1, argv + 1);
    }

    std::cerr << program << ": unknown command '" << name << "'\n";
    print_usage(program);
    return 2;
}
