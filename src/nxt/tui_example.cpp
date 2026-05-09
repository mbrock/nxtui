#include "nxtio/app.hpp"
#include "nxtio/async.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"

#include <format>
#include <random>
#include <vector>

namespace nxt::tui_example {

using namespace nxt::tui;

struct Activity
{
    std::string label;
    percent_t progress{0.0 * percent};
};

// Fake log messages for demo
const std::vector<std::string> fake_logs = {
    "Fetching source from cache.nixos.org...",
    "Unpacking source archive...",
    "Patching sources...",
    "Running configure phase...",
    "Compiling src/main.cpp",
    "Compiling src/parser.cpp",
    "Compiling src/codegen.cpp",
    "Linking libfoo.so",
    "Running test suite...",
    "Installing to /nix/store/...",
    "Registering outputs...",
    "Build completed successfully",
    "Checking derivation inputs...",
    "Downloading https://github.com/...",
    "Verifying SHA256 hash...",
    "Extracting archive...",
    "Applying patch fix-build.patch",
    "Configuring with cmake...",
    "Building target: all",
    "Post-install fixup phase...",
};

int example(int argc, char * argv[])
{
    using namespace std::chrono_literals;
    using namespace nxt::tui_example;
    using nxt::percent;

    int steps = 100;
    if (argc > 1 && std::string_view(argv[1]) == "--steps")
        steps = std::stoi(argv[2]);

    return nxt::ui::run(
        std::vector<Activity>({
            {"nixpkgs.tar.gz"},
            {"rustc.tar.xz"},
            {"llvm-17.src.tar.xz"},
        }),
        [](auto & state) {
            return column(
                list(
                    state,
                    [](const auto & act) {
                        return row(
                            text(
                                std::format("{:<20}", act.label),
                                act.progress >= 100 * percent
                                    ? (fg(Rgba8::green()) | bold)
                                    : fg(Rgba8::blue())),
                            progress_bar(act.progress),
                            text(
                                std::format(
                                    "{:>4.0f}%",
                                    act.progress.value()),
                                (act.progress >= 1 * one
                                     ? fg(Rgba8::green())
                                 : act.progress >= 0.5 * one
                                     ? fg(Rgba8::yellow())
                                     : fg(Rgba8::white()))
                                    | bold));
                    })
            );
        },
        [n = steps](
            nxt::ui::UIRuntime & runtime,
            std::vector<Activity> & state) -> nxt::task<> {
            std::mt19937 rng(42);
            std::uniform_int_distribution<std::size_t> log_dist(
                0, fake_logs.size() - 1);

            for (int frame = 0; frame <= n; ++frame) {
                if (runtime.shutdown_requested())
                    co_return;

                auto x = frame * percent / (n / 100.0);

                state[0].progress = std::min(x / 0.8, 100.0 * percent);
                state[1].progress = std::min(x / 0.6, 100.0 * percent);
                state[2].progress = std::min(x / 0.4, 100.0 * percent);

                // Print a fake log message every few frames
                if (frame % 5 == 0) {
                    auto & msg = fake_logs[log_dist(rng)];
                    runtime.println(std::format("[{:3}] {}", frame, msg));
                }

                co_await runtime.scheduler().yield_for(32ms);
            }

            runtime.println("All builds completed!");
            co_await runtime.scheduler().yield_for(1s);
            runtime.request_shutdown();
        });
}

} // namespace nxt::tui_example

int main(int argc, char * argv[])
{
    return nxt::tui_example::example(argc, argv);
}
