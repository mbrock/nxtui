#include <nxt/ansi.hpp>
#include <nxt/any_layout.hpp>
#include <nxt/compositor.hpp>
#include <nxt/glyph-table.hpp>
#include <nxt/rt/app.hpp>
#include <nxt/rt/buffers.hpp>
#include <nxt/rt/cgroup.hpp>
#include <nxt/rt/pty.hpp>
#include <nxt/tui.hpp>

#include <algorithm>
#include <array>
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
#include <span>
#include <string>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using sample = nxt::rt::cgroup::sample;
using bytes_t = nxt::rt::cgroup::bytes_t;
using usec_t = nxt::rt::cgroup::usec_t;

struct session_state
{
    std::string unit_name;
    std::filesystem::path cgroup_path;
    std::deque<sample> samples;
    nxt::rt::child_result status;
    bool process_done = false;

    void push_sample(sample value)
    {
        samples.push_back(std::move(value));
        while (samples.size() > 64)
            samples.pop_front();
    }
};

class raw_terminal
{
public:
    explicit raw_terminal(int fd)
        : fd_(fd)
        , active_(::isatty(fd) && ::tcgetattr(fd, &saved_) == 0)
    {
        if (!active_)
            return;

        auto raw = saved_;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(fd_, TCSANOW, &raw) != 0)
            active_ = false;
    }

    raw_terminal(const raw_terminal &) = delete;
    raw_terminal & operator=(const raw_terminal &) = delete;

    ~raw_terminal()
    {
        if (active_)
            (void)::tcsetattr(fd_, TCSANOW, &saved_);
    }

private:
    int fd_ = -1;
    termios saved_{};
    bool active_ = false;
};

nxt::Size terminal_size()
{
    auto ws = winsize{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
        && ws.ws_col > 0
        && ws.ws_row > 0) {
        return nxt::Size{
            static_cast<std::size_t>(ws.ws_col) * nxt::ch,
            static_cast<std::size_t>(ws.ws_row) * nxt::ln,
        };
    }
    return nxt::Size{96 * nxt::ch, 26 * nxt::ln};
}

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

std::string format_time(usec_t u)
{
    auto d = static_cast<double>(u.v);
    if (d >= 1e6)
        return std::format("{:.2f}s", d / 1e6);
    if (d >= 1e3)
        return std::format("{:.0f}ms", d / 1e3);
    return std::format("{}us", u.v);
}

std::string make_unit_name()
{
    auto now = std::chrono::system_clock::now();
    auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                     now.time_since_epoch())
                     .count();
    auto rng = std::mt19937_64{static_cast<std::uint64_t>(stamp)};
    return std::format("nxt-ng-shell-{}-{:06x}", ::getpid(), rng() & 0xffffff);
}

std::string sparkline(std::span<const double> values, std::size_t cells)
{
    static constexpr auto blocks = std::array<std::string_view, 9>{
        " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█",
    };
    if (cells == 0)
        return {};
    if (values.empty())
        return std::string(cells, ' ');

    auto [lo_it, hi_it] = std::minmax_element(values.begin(), values.end());
    auto lo = *lo_it;
    auto hi = *hi_it;
    if (std::abs(hi - lo) < 1e-9)
        hi = lo + 1.0;

    auto out = std::string{};
    auto take = std::min(values.size(), cells);
    out.append(cells - take, ' ');
    auto offset = values.size() - take;
    for (std::size_t i = 0; i != take; ++i) {
        auto frac = (values[offset + i] - lo) / (hi - lo);
        auto idx = static_cast<std::size_t>(
            std::round(std::clamp(frac, 0.0, 1.0) * 8.0));
        out += blocks[idx];
    }
    return out;
}

double cpu_percent(const session_state & state)
{
    if (state.samples.size() < 2)
        return 0.0;
    auto const & a = state.samples[state.samples.size() - 2];
    auto const & b = state.samples.back();
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

std::string cpu_spark(const session_state & state)
{
    auto hist = std::vector<double>{};
    for (std::size_t i = 1; i < state.samples.size(); ++i) {
        auto const & a = state.samples[i - 1];
        auto const & b = state.samples[i];
        auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                      b.at - a.at)
                      .count();
        auto dcpu = static_cast<std::int64_t>(b.cpu_usage.v)
                    - static_cast<std::int64_t>(a.cpu_usage.v);
        hist.push_back(
            dt > 0
                ? 100.0
                      * static_cast<double>(
                          std::max<std::int64_t>(0, dcpu))
                      / static_cast<double>(dt)
                : 0.0);
    }
    return sparkline(hist, 24);
}

std::string child_status_text(const session_state & state)
{
    if (!state.process_done)
        return "running";
    if (state.status.exited)
        return std::format("exited {}", state.status.exit_code);
    if (state.status.signaled)
        return std::format("signal {}", state.status.signal);
    return "done";
}

auto frame_layout(
    const session_state & state,
    nxt::rt::pty::session & pty,
    nxt::Size size)
{
    auto width = std::max<std::size_t>(32, size.w.count());
    auto sample_count = state.samples.size();
    auto latest = sample_count == 0 ? sample{} : state.samples.back();
    auto scope = state.cgroup_path.empty() ? std::string{"discovering scope..."}
                                           : state.cgroup_path.string();
    if (scope.size() > width - 8)
        scope = "..." + scope.substr(scope.size() - (width - 11));

    return nxt::tui::column(
        nxt::tui::row(
            nxt::tui::text(" ng shell scope ", nxt::tui::bold),
            nxt::tui::flex_fill(nxt::Rgba8{36, 42, 52})),
        nxt::tui::text(
            "unit " + state.unit_name + "  status " + child_status_text(state),
            nxt::tui::fg(nxt::Rgba8::bright_cyan())),
        nxt::tui::text(scope, nxt::tui::fg(nxt::Rgba8{150, 160, 178})),
        nxt::tui::hrule(),
        nxt::tui::text(
            "memory " + format_bytes(latest.memory_current) + "  peak "
                + format_bytes(latest.memory_peak) + "  pids "
                + std::to_string(latest.pids.v),
            nxt::tui::fg(nxt::Rgba8{230, 205, 130})),
        nxt::tui::text(
            "cpu " + std::format("{:.1f}%", cpu_percent(state)) + "  "
                + cpu_spark(state) + "  total " + format_time(latest.cpu_usage)
                + " user " + format_time(latest.cpu_user) + " sys "
                + format_time(latest.cpu_system),
            nxt::tui::fg(nxt::Rgba8{160, 210, 150})),
        nxt::tui::text(
            std::format(
                "pressure avg10  mem {:.2f}%  cpu {:.2f}%  io {:.2f}%",
                latest.psi_mem,
                latest.psi_cpu,
                latest.psi_io),
            nxt::tui::fg(nxt::Rgba8{170, 180, 210})),
        nxt::tui::hrule(),
        nxt::rt::pty::pty_screen(
            pty,
            nxt::tui::bg(nxt::Rgba8{12, 14, 18})));
}

void render_frame(
    nxt::ui::TerminalCompositor & compositor,
    const session_state & state,
    nxt::rt::pty::session & pty,
    nxt::Size size)
{
    auto & buffer = compositor.back_buffer();
    buffer.clear();
    auto layout = frame_layout(state, pty, size);
    auto view = buffer.view();
    layout.render(view, size);
    compositor.present_frame(std::cout);
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

    auto size = terminal_size();
    auto pty = co_await nxt::rt::pty::spawn(nxt::rt::pty::spawn_options{
        .argv = std::move(argv),
        .size = size,
    });
    auto glyphs = nxt::GlyphTable{};
    auto compositor = nxt::ui::TerminalCompositor{size, glyphs};
    auto raw = raw_terminal{STDIN_FILENO};

    std::cout << "\x1b[?25l\x1b[2J\x1b[H" << std::flush;
    auto failure = std::exception_ptr{};
    try {
        co_await nxt::rt::with_zone([&]() -> nxt::rt::task<void> {
            auto reader =
                nxt::rt::fork(read_pty_until_done(pty, state)).cope();
            auto input =
                nxt::rt::fork(pump_stdin_to_pty(pty, state)).cope();

            while (!state.process_done) {
                if (state.cgroup_path.empty()) {
                    if (auto found =
                            co_await nxt::rt::cgroup::find_unit_scope(
                                state.unit_name))
                        state.cgroup_path = std::move(*found);
                }
                if (!state.cgroup_path.empty())
                    state.push_sample(
                        co_await nxt::rt::cgroup::read_sample(
                            state.cgroup_path));

                auto next_size = terminal_size();
                if (next_size.w != size.w || next_size.h != size.h) {
                    size = next_size;
                    compositor.resize(size);
                }
                render_frame(compositor, state, pty, size);
                co_await nxt::rt::op::timeout::after(80ms);
            }

            if (!state.cgroup_path.empty() && std::filesystem::exists(state.cgroup_path))
                state.push_sample(
                    co_await nxt::rt::cgroup::read_sample(state.cgroup_path));
            render_frame(compositor, state, pty, size);

            auto reader_done = std::move(reader).get();
            if (!reader_done)
                nxt::rt::rethrow(reader_done.error());
            auto input_done = std::move(input).get();
            if (!input_done)
                nxt::rt::rethrow(input_done.error());
        });
    } catch (...) {
        std::cout << "\x1b[?25h\x1b[0m" << std::flush;
        failure = std::current_exception();
    }

    if (failure) {
        if (!state.process_done)
            co_await pty.terminate_and_wait();
        nxt::rt::rethrow(failure);
    }

    std::cout << "\x1b[?25h\x1b[0m\n" << std::flush;
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
    std::cout << "\x1b[?25h\x1b[0m" << std::flush;
    std::cerr << "ng-shell-scope-demo: " << error.what() << '\n';
    return 1;
}
