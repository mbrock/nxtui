#include <nxt/ansi.hpp>
#include <nxt/any_layout.hpp>
#include <nxt/compositor.hpp>
#include <nxt/glyph-table.hpp>
#include <nxt/rt/app.hpp>
#include <nxt/rt/buffers.hpp>
#include <nxt/rt/cgroup.hpp>
#include <nxt/rt/pty.hpp>
#include <nxt/rt/terminal_app.hpp>
#include <nxt/tui.hpp>
#include <nxt/tui_sparkline.hpp>

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

using sample = nxt::rt::cgroup::sample;
using bytes_t = nxt::rt::cgroup::bytes_t;
using usec_t = nxt::rt::cgroup::usec_t;

struct session_state
{
    std::string unit_name;
    std::filesystem::path cgroup_path;
    std::deque<sample> samples;
    std::vector<double> memory_points;
    std::vector<double> cpu_points;
    std::optional<sample> last_sparkline_sample;
    nxt::rt::child_result status;
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
    constexpr auto value_width = 20 * nxt::ch;
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

    return nxt::tui::column(
        nxt::tui::row(
            nxt::tui::fixed_width(
                value_width,
                nxt::tui::text(
                    std::move(mem_line),
                    nxt::tui::fg(nxt::Rgba8{230, 205, 130}))),
            nxt::tui::text("  "),
            nxt::tui::sparkline(
                std::span<const double>{state.memory_points},
                2 * nxt::ln,
                nxt::tui::fg(nxt::Rgba8{225, 175, 105}),
                nxt::chart::value_range{0.0, mem_peak})),
        nxt::tui::row(
            nxt::tui::fixed_width(
                value_width,
                nxt::tui::text(
                    std::move(cpu_line),
                    nxt::tui::fg(nxt::Rgba8{160, 210, 150}))),
            nxt::tui::text("  "),
            nxt::tui::sparkline(
                std::span<const double>{state.cpu_points},
                2 * nxt::ln,
                nxt::tui::fg(nxt::Rgba8{105, 190, 170}),
                nxt::chart::value_range{0.0, 100.0})));
}

auto frame_layout(
    const session_state & state,
    nxt::rt::pty::session & pty)
{
    return nxt::tui::column(
        metrics_layout(state),
        nxt::rt::pty::pty_screen(
            pty,
            nxt::tui::bg(nxt::Rgba8{12, 14, 18})));
}

void render_frame(
    nxt::rt::terminal_app & terminal,
    const session_state & state,
    nxt::rt::pty::session & pty)
{
    auto & compositor = terminal.compositor();
    auto & buffer = compositor.back_buffer();
    buffer.clear();
    auto layout = frame_layout(state, pty);
    auto view = buffer.view();
    layout.render(view, terminal.size());
    compositor.present_frame(std::cout);
}

nxt::rt::task<void> sample_cgroup_until_done(session_state & state)
{
    auto next_sample =
        std::chrono::steady_clock::now() + cgroup_sample_interval;
    auto next_sparkline_sample = std::chrono::steady_clock::now();

    while (!state.process_done) {
        if (state.cgroup_path.empty()) {
            if (auto found =
                    co_await nxt::rt::cgroup::find_unit_scope(
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
                co_await nxt::rt::cgroup::read_sample(state.cgroup_path),
                include_sparkline);
        }

        now = std::chrono::steady_clock::now();
        if (now < next_sample)
            co_await nxt::rt::op::timeout::after(next_sample - now);
        next_sample += cgroup_sample_interval;
        if (next_sample < now)
            next_sample = now + cgroup_sample_interval;
    }

    if (!state.cgroup_path.empty()
        && std::filesystem::exists(state.cgroup_path))
        state.push_sample(
            co_await nxt::rt::cgroup::read_sample(state.cgroup_path),
            true);
}

nxt::rt::task<void> render_until_done(
    nxt::rt::terminal_app & terminal,
    const session_state & state,
    nxt::rt::pty::session & pty)
{
    auto next_frame = std::chrono::steady_clock::now() + frame_interval;

    while (!state.process_done) {
        (void)terminal.refresh_size();
        render_frame(terminal, state, pty);

        auto now = std::chrono::steady_clock::now();
        if (now < next_frame)
            co_await nxt::rt::op::timeout::after(next_frame - now);
        next_frame += frame_interval;
        if (next_frame < now)
            next_frame = now + frame_interval;
    }

    (void)terminal.refresh_size();
    render_frame(terminal, state, pty);
}

nxt::rt::task<void> read_pty_until_done(
    nxt::rt::pty::session & pty,
    session_state & state)
{
    state.status = co_await pty.read_loop();
    state.process_done = true;
}

nxt::rt::task<void> pump_stdin_to_pty(
    nxt::rt::pty::session & pty,
    const session_state & state)
{
    auto storage = std::array<std::byte, 4096>{};

    while (!state.process_done) {
        auto ready = co_await nxt::rt::op::poll_until::after(
            STDIN_FILENO,
            POLLIN,
            80ms);
        if (ready.timed_out)
            continue;
        if ((ready.events & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            co_return;
        if ((ready.events & POLLIN) == 0)
            continue;

        auto n = co_await nxt::rt::op::read_some{
            .fd = STDIN_FILENO,
            .buffer = std::span{storage},
        };
        if (n == 0)
            co_return;

        co_await pty.write_all(
            std::string{nxt::rt::as_string_view(std::span{storage}.first(n))});
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

nxt::rt::task<void> run_scoped_command(std::string command = {})
{
    nxt::ansi::init();
    nxt::ansi::mode = nxt::ansi::Mode::enabled;

    auto state = session_state{};
    state.unit_name = make_unit_name();
    auto argv = scoped_shell_argv(state.unit_name, command);

    auto size = nxt::rt::current_terminal_size();
    auto pty = co_await nxt::rt::pty::spawn(nxt::rt::pty::spawn_options{
        .argv = std::move(argv),
        .size = size,
    });
    auto terminal = nxt::rt::terminal_app{};

    auto failure = std::exception_ptr{};
    auto reader = nxt::rt::catching_deed<void>{};
    auto input = nxt::rt::catching_deed<void>{};
    auto sampler = nxt::rt::catching_deed<void>{};
    auto renderer = nxt::rt::catching_deed<void>{};
    try {
        co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
            reader = nxt::rt::fork(read_pty_until_done(pty, state)).cope();
            input = nxt::rt::fork(pump_stdin_to_pty(pty, state)).cope();
            sampler = nxt::rt::fork(sample_cgroup_until_done(state)).cope();
            renderer =
                nxt::rt::fork(render_until_done(terminal, state, pty)).cope();

            while (!state.process_done)
                co_await nxt::rt::op::timeout::after(frame_interval);
        });
    } catch (...) {
        failure = std::current_exception();
    }

    if (failure) {
        if (!state.process_done)
            co_await pty.terminate_and_wait();
        nxt::rt::rethrow(failure);
    }

    auto reader_done = std::move(reader).get();
    if (!reader_done)
        nxt::rt::rethrow(reader_done.error());
    auto input_done = std::move(input).get();
    if (!input_done)
        nxt::rt::rethrow(input_done.error());
    auto sampler_done = std::move(sampler).get();
    if (!sampler_done)
        nxt::rt::rethrow(sampler_done.error());
    auto renderer_done = std::move(renderer).get();
    if (!renderer_done)
        nxt::rt::rethrow(renderer_done.error());
}

} // namespace

int main(int argc, char ** argv)
try {
    auto command = std::string{};
    if (argc > 1) {
        for (int i = 1; i != argc; ++i) {
            if (!command.empty())
                command += ' ';
            command += argv[i];
        }
    }

    auto rt = nxt::rt::runtime{};
    rt.run(run_scoped_command(std::move(command)));
    return 0;
} catch (std::exception const & error) {
    std::cout << "\x1b[?25h\x1b[0m\x1b[?1049l" << std::flush;
    std::cerr << "nxt-shell-scope-demo: " << error.what() << '\n';
    return 1;
}
