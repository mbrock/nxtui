#include <nxt/ansi.hpp>
#include <nxt/compositor.hpp>
#include <nxt/glyph-table.hpp>
#include <nxt/rt/app.hpp>
#include <nxt/tui.hpp>

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

    return nxt::Size{72 * nxt::ch, 14 * nxt::ln};
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

auto frame_layout(int tick, nxt::Size size)
{
    auto width = std::max<std::size_t>(16, size.w.count());
    auto bar = ((tick * 7) % 101) * nxt::percent;
    auto pulse = sparkline(tick, std::min<std::size_t>(width, 64));

    return nxt::tui::column(
        nxt::tui::row(
            nxt::tui::text(" nxt::rt TUI demo ", nxt::tui::bold),
            nxt::tui::flex_fill(nxt::Rgba8::bright_black())),
        nxt::tui::hrule(),
        nxt::tui::text(
            "running without libcoro: deck + wand + root zone",
            nxt::tui::fg(nxt::Rgba8::bright_cyan())),
        nxt::tui::text(
            "frame " + std::to_string(tick)
                + "  size " + std::to_string(size.w.count())
                + "x" + std::to_string(size.h.count())),
        nxt::tui::progress_bar(
            bar,
            nxt::Rgba8::bright_green(),
            nxt::Rgba8::bright_black()),
        nxt::tui::text(pulse, nxt::tui::fg(nxt::Rgba8::bright_magenta())),
        nxt::tui::hrule(),
        nxt::tui::text("this is the tiny first TUI island on nxt::rt"));
}

void render_frame(
    nxt::ui::TerminalCompositor & compositor,
    int tick,
    nxt::Size size)
{
    auto & buffer = compositor.back_buffer();
    buffer.clear();

    auto layout = frame_layout(tick, size);
    auto view = buffer.view();
    layout.render(view, size);
    compositor.present_frame(std::cout);
}

nxt::rt::task<void> run_demo()
{
    nxt::ansi::init();
    nxt::ansi::mode = nxt::ansi::Mode::enabled;

    auto size = terminal_size();
    auto glyphs = nxt::GlyphTable{};
    auto compositor = nxt::ui::TerminalCompositor{size, glyphs};

    std::cout << "\x1b[?25l\x1b[2J\x1b[H" << std::flush;
    try {
        for (int tick = 0; tick != 80; ++tick) {
            auto next_size = terminal_size();
            if (next_size.w != size.w || next_size.h != size.h) {
                size = next_size;
                compositor.resize(size);
            }

            render_frame(compositor, tick, size);
            co_await nxt::rt::op::timeout::after(50ms);
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
    auto rt = nxt::rt::runtime{};
    rt.run(run_demo());
    return 0;
} catch (std::exception const & error) {
    std::cout << "\x1b[?25h\x1b[0m" << std::flush;
    std::cerr << "ng-tui-demo: " << error.what() << '\n';
    return 1;
}
