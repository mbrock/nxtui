#include <nxtui/ansi.hpp>
#include <nxtui/compositor.hpp>
#include <nxtui/glyph-table.hpp>
#include <nxtrt/app.hpp>
#include <nxtui/tui.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

nxtui::Size terminal_size()
{
    auto ws = winsize{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
        && ws.ws_col > 0
        && ws.ws_row > 0) {
        return nxtui::Size{
            static_cast<std::size_t>(ws.ws_col) * nxtui::ch,
            static_cast<std::size_t>(ws.ws_row) * nxtui::ln,
        };
    }

    return nxtui::Size{72 * nxtui::ch, 14 * nxtui::ln};
}

std::string sparkline(int tick, std::size_t width)
{
    static constexpr auto blocks = std::array<std::string_view, 8>{
        "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█",
    };

    auto out = std::string{};
    out.reserve(width * 3);
    for (auto i = std::size_t{}; i != width; ++i) {
        auto idx = (i + static_cast<std::size_t>(tick)) % blocks.size();
        out += blocks[idx];
    }
    return out;
}

auto frame_layout(int tick, nxtui::Size size)
{
    auto width = std::max<std::size_t>(16, size.w.count());
    auto bar = ((tick * 7) % 101) * nxtui::percent;
    auto pulse = sparkline(tick, std::min<std::size_t>(width, 64));

    return nxtui::tui::column(
        nxtui::tui::row(
            nxtui::tui::text(" nxtrt TUI demo ", nxtui::tui::bold),
            nxtui::tui::flex_fill(nxtui::Rgba8::bright_black())),
        nxtui::tui::hrule(),
        nxtui::tui::text(
            "running without libcoro: deck + wand + root zone",
            nxtui::tui::fg(nxtui::Rgba8::bright_cyan())),
        nxtui::tui::text(
            "frame " + std::to_string(tick)
                + "  size " + std::to_string(size.w.count())
                + "x" + std::to_string(size.h.count())),
        nxtui::tui::progress_bar(
            bar,
            nxtui::Rgba8::bright_green(),
            nxtui::Rgba8::bright_black()),
        nxtui::tui::text(pulse, nxtui::tui::fg(nxtui::Rgba8::bright_magenta())),
        nxtui::tui::hrule(),
        nxtui::tui::text("this is the tiny first TUI island on nxtrt"));
}

void render_frame(
    nxtui::tui::TerminalCompositor & compositor,
    int tick,
    nxtui::Size size)
{
    auto & buffer = compositor.back_buffer();
    buffer.clear();

    auto layout = frame_layout(tick, size);
    auto view = buffer.view();
    layout.render(view, size);
    compositor.present_frame(std::cout);
}

nxtrt::task<void> run_demo()
{
    nxtui::ansi::init();
    nxtui::ansi::mode = nxtui::ansi::Mode::enabled;

    auto size = terminal_size();
    auto glyphs = nxtui::GlyphTable{};
    auto compositor = nxtui::tui::TerminalCompositor{size, glyphs};

    std::cout << "\x1b[?25l\x1b[2J\x1b[H" << std::flush;
    try {
        for (int tick = 0; tick != 80; ++tick) {
            auto next_size = terminal_size();
            if (next_size.w != size.w || next_size.h != size.h) {
                size = next_size;
                compositor.resize(size);
            }

            render_frame(compositor, tick, size);
            co_await nxtrt::op::timeout::after(50ms);
        }
    } catch (...) {
        std::cout << "\x1b[?25h\x1b[0m" << std::flush;
        throw;
    }

    std::cout << "\x1b[?25h\x1b[0m" << std::flush;
}

} // namespace

int main()
try {
    auto rt = nxtrt::runtime{};
    rt.run(run_demo());
    return 0;
} catch (std::exception const & error) {
    std::cout << "\x1b[?25h\x1b[0m" << std::flush;
    std::cerr << "nxt-tui-demo: " << error.what() << '\n';
    return 1;
}
