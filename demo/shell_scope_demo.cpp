#include <nxtui/ansi.hpp>
#include <nxtui/any_layout.hpp>
#include <nxtui/compositor.hpp>
#include <nxtui/glyph-table.hpp>
#include <nxtrt/app.hpp>
#include <nxtrt/buffers.hpp>
#include <nxtrt/cgroup.hpp>
#include <nxtrt/pty.hpp>
#include <nxtrt/terminal_app.hpp>
#include <nxtui/tui.hpp>
#include <nxtui/tui_sparkline.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <poll.h>
#include <random>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr auto frame_interval = 16ms;
constexpr auto cgroup_sample_interval = 33ms;
constexpr auto cgroup_sparkline_interval = 120ms;
constexpr auto cgroup_live_samples = std::size_t{8};
constexpr auto cgroup_sparkline_samples = std::size_t{160};
constexpr auto memory_chart_floor_bytes = std::uint64_t{
    128ull * 1024ull * 1024ull};

using sample = nxtrt::cgroup::sample;
using bytes_t = nxtrt::cgroup::bytes_t;
using usec_t = nxtrt::cgroup::usec_t;

struct session_state
{
    std::string unit_name;
    std::filesystem::path cgroup_path;
    std::deque<sample> samples;
    std::vector<double> memory_points;
    std::vector<double> cpu_points;
    std::optional<sample> last_sparkline_sample;
    nxtrt::child_result status;
    bool process_done = false;

    session_state()
    {
        memory_points.reserve(cgroup_sparkline_samples);
        cpu_points.reserve(cgroup_sparkline_samples);
    }

    void push_sample(sample value, bool include_sparkline = false)
    {
        if (include_sparkline)
            push_sparkline_sample(value);

        samples.push_back(std::move(value));
        while (samples.size() > cgroup_live_samples)
            samples.pop_front();
    }

    void push_sparkline_sample(const sample & value);
};

std::string format_bytes(bytes_t b)
{
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;
    auto d = static_cast<double>(b.v);
    if (d >= gib)
        return std::format("{:.2f} GiB", d / gib);
    if (d >= mib)
        return std::format("{:.1f} MiB", d / mib);
    if (d >= kib)
        return std::format("{:.0f} KiB", d / kib);
    return std::format("{} B", b.v);
}

std::string make_unit_name()
{
    auto now = std::chrono::system_clock::now();
    auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                     now.time_since_epoch())
                     .count();
    auto rng = std::mt19937_64{static_cast<std::uint64_t>(stamp)};
    return std::format("nxt-shell-{}-{:06x}", ::getpid(), rng() & 0xffffff);
}

double cpu_percent_between(const sample & a, const sample & b)
{
    auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                  b.at - a.at)
                  .count();
    if (dt <= 0)
        return 0.0;
    auto dcpu = static_cast<std::int64_t>(b.cpu_usage.v)
                - static_cast<std::int64_t>(a.cpu_usage.v);
    return 100.0 * static_cast<double>(std::max<std::int64_t>(0, dcpu))
           / static_cast<double>(dt);
}

void session_state::push_sparkline_sample(const sample & value)
{
    memory_points.push_back(static_cast<double>(value.memory_current.v));
    if (last_sparkline_sample)
        cpu_points.push_back(cpu_percent_between(*last_sparkline_sample, value));
    last_sparkline_sample = value;

    if (memory_points.size() > cgroup_sparkline_samples)
        memory_points.erase(memory_points.begin());
    if (cpu_points.size() > cgroup_sparkline_samples)
        cpu_points.erase(cpu_points.begin());
}

double cpu_percent(const session_state & state)
{
    if (state.samples.size() < 2)
        return 0.0;
    auto const & a = state.samples[state.samples.size() - 2];
    auto const & b = state.samples.back();
    return cpu_percent_between(a, b);
}

auto metrics_layout(const session_state & state)
{
    constexpr auto value_width = 20 * nxtui::ch;
    auto latest = state.samples.empty() ? sample{} : state.samples.back();
    auto cpu = cpu_percent(state);
    auto mem_peak = latest.memory_peak.v == 0
        ? static_cast<double>(memory_chart_floor_bytes)
        : static_cast<double>(
              std::max(latest.memory_peak.v, memory_chart_floor_bytes));
    auto mem_line = std::format(
        "{} / {}",
        format_bytes(latest.memory_current),
        format_bytes(latest.memory_peak));
    auto cpu_line = std::format("{:.0f}% CPU", cpu);

    return nxtui::tui::column(
        nxtui::tui::row(
            nxtui::tui::fixed_width(
                value_width,
                nxtui::tui::text(
                    std::move(mem_line),
                    nxtui::tui::fg(nxtui::Rgba8{230, 205, 130}))),
            nxtui::tui::text("  "),
            nxtui::tui::sparkline(
                std::span<const double>{state.memory_points},
                2 * nxtui::ln,
                nxtui::tui::fg(nxtui::Rgba8{225, 175, 105}),
                nxtui::chart::value_range{0.0, mem_peak})),
        nxtui::tui::row(
            nxtui::tui::fixed_width(
                value_width,
                nxtui::tui::text(
                    std::move(cpu_line),
                    nxtui::tui::fg(nxtui::Rgba8{160, 210, 150}))),
            nxtui::tui::text("  "),
            nxtui::tui::sparkline(
                std::span<const double>{state.cpu_points},
                2 * nxtui::ln,
                nxtui::tui::fg(nxtui::Rgba8{105, 190, 170}),
                nxtui::chart::value_range{0.0, 100.0})));
}

auto frame_layout(
    const session_state & state,
    nxtrt::pty::session & pty)
{
    return nxtui::tui::column(
        metrics_layout(state),
        nxtrt::pty::pty_screen(
            pty,
            nxtui::tui::bg(nxtui::Rgba8{12, 14, 18})));
}

void render_frame(
    nxtrt::terminal_app & terminal,
    const session_state & state,
    nxtrt::pty::session & pty)
{
    auto & compositor = terminal.compositor();
    auto & buffer = compositor.back_buffer();
    buffer.clear();
    auto layout = frame_layout(state, pty);
    auto view = buffer.view();
    layout.render(view, terminal.size());
    compositor.present_frame(std::cout);
}

nxtrt::task<void> sample_cgroup_until_done(session_state & state)
{
    auto next_sample =
        std::chrono::steady_clock::now() + cgroup_sample_interval;
    auto next_sparkline_sample = std::chrono::steady_clock::now();

    while (!state.process_done) {
        if (state.cgroup_path.empty()) {
            if (auto found =
                    co_await nxtrt::cgroup::find_unit_scope(
                        state.unit_name))
                state.cgroup_path = std::move(*found);
        }

        auto now = std::chrono::steady_clock::now();
        if (!state.cgroup_path.empty()) {
            auto include_sparkline = now >= next_sparkline_sample;
            if (include_sparkline)
                while (next_sparkline_sample <= now)
                    next_sparkline_sample += cgroup_sparkline_interval;
            state.push_sample(
                co_await nxtrt::cgroup::read_sample(state.cgroup_path),
                include_sparkline);
        }

        now = std::chrono::steady_clock::now();
        if (now < next_sample)
            co_await nxtrt::op::timeout::after(next_sample - now);
        next_sample += cgroup_sample_interval;
        if (next_sample < now)
            next_sample = now + cgroup_sample_interval;
    }

    if (!state.cgroup_path.empty()
        && std::filesystem::exists(state.cgroup_path))
        state.push_sample(
            co_await nxtrt::cgroup::read_sample(state.cgroup_path),
            true);
}

nxtrt::task<void> render_until_done(
    nxtrt::terminal_app & terminal,
    const session_state & state,
    nxtrt::pty::session & pty)
{
    auto next_frame = std::chrono::steady_clock::now() + frame_interval;

    while (!state.process_done) {
        (void)terminal.refresh_size();
        render_frame(terminal, state, pty);

        auto now = std::chrono::steady_clock::now();
        if (now < next_frame)
            co_await nxtrt::op::timeout::after(next_frame - now);
        next_frame += frame_interval;
        if (next_frame < now)
            next_frame = now + frame_interval;
    }

    (void)terminal.refresh_size();
    render_frame(terminal, state, pty);
}

nxtrt::task<void> read_pty_until_done(
    nxtrt::pty::session & pty,
    session_state & state)
{
    state.status = co_await pty.read_loop();
    state.process_done = true;
}

nxtrt::task<void> pump_stdin_to_pty(
    nxtrt::pty::session & pty,
    const session_state & state)
{
    auto storage = std::array<std::byte, 4096>{};

    while (!state.process_done) {
        auto ready = co_await nxtrt::poll_until_after(
            STDIN_FILENO,
            POLLIN,
            80ms);
        if (ready.timed_out)
            continue;
        if ((ready.events & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            co_return;
        if ((ready.events & POLLIN) == 0)
            continue;

        auto n = co_await nxtrt::op::read_some{
            .fd = STDIN_FILENO,
            .buffer = std::span{storage},
        };
        if (n == 0)
            co_return;

        co_await pty.write_all(
            std::string{nxtrt::as_string_view(std::span{storage}.first(n))});
    }
}

std::vector<std::string> scoped_shell_argv(
    const std::string & unit_name,
    const std::string & command)
{
    auto argv = std::vector<std::string>{
        "systemd-run",
        "--user",
        "--scope",
        "--quiet",
        "--collect",
        std::format("--unit={}", unit_name),
        "/bin/bash",
    };
    if (command.empty()) {
        argv.emplace_back("-l");
    } else {
        argv.emplace_back("-lc");
        argv.push_back(command);
    }
    return argv;
}

nxtrt::task<void> run_scoped_command(std::string command = {})
{
    nxtui::ansi::init();
    nxtui::ansi::mode = nxtui::ansi::Mode::enabled;

    auto state = session_state{};
    state.unit_name = make_unit_name();
    auto argv = scoped_shell_argv(state.unit_name, command);

    auto size = nxtrt::current_terminal_size();
    auto pty = co_await nxtrt::pty::spawn(nxtrt::pty::spawn_options{
        .argv = std::move(argv),
        .size = size,
    });
    auto terminal = nxtrt::terminal_app{};

    auto failure = std::exception_ptr{};
    auto reader = nxtrt::catching_deed<void>{};
    auto input = nxtrt::catching_deed<void>{};
    auto sampler = nxtrt::catching_deed<void>{};
    auto renderer = nxtrt::catching_deed<void>{};
    try {
        co_await nxtrt::with_firm([&]() -> nxtrt::task<void> {
            reader = nxtrt::fork(read_pty_until_done(pty, state)).cope();
            input = nxtrt::fork(pump_stdin_to_pty(pty, state)).cope();
            sampler = nxtrt::fork(sample_cgroup_until_done(state)).cope();
            renderer =
                nxtrt::fork(render_until_done(terminal, state, pty)).cope();

            while (!state.process_done)
                co_await nxtrt::op::timeout::after(frame_interval);
            co_await nxtrt::join();
        });
    } catch (...) {
        failure = std::current_exception();
    }

    if (failure) {
        if (!state.process_done)
            co_await pty.terminate_and_wait();
        nxtrt::rethrow(failure);
    }

    auto reader_done = std::move(reader).get();
    if (!reader_done)
        nxtrt::rethrow(reader_done.error());
    auto input_done = std::move(input).get();
    if (!input_done)
        nxtrt::rethrow(input_done.error());
    auto sampler_done = std::move(sampler).get();
    if (!sampler_done)
        nxtrt::rethrow(sampler_done.error());
    auto renderer_done = std::move(renderer).get();
    if (!renderer_done)
        nxtrt::rethrow(renderer_done.error());
}

} // namespace

int nxt_shell_scope_demo_main(int argc, char ** argv)
try {
    auto command = std::string{};
    if (argc > 1) {
        for (int i = 1; i != argc; ++i) {
            if (!command.empty())
                command += ' ';
            command += argv[i];
        }
    }

    auto rt = nxtrt::runtime{};
    rt.run(run_scoped_command(std::move(command)));
    return 0;
} catch (std::exception const & error) {
    std::cout << "\x1b[?25h\x1b[0m\x1b[?1049l" << std::flush;
    std::cerr << "nxt-shell-scope-demo: " << error.what() << '\n';
    return 1;
}

#if !defined(NXT_EMBEDDED_MAIN)
int main(int argc, char ** argv)
{
    return nxt_shell_scope_demo_main(argc, argv);
}
#endif
