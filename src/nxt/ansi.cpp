#include "nxt/ansi.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"

#include <cstdio>
#include <format>
#include <iostream>
#include <iterator>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace nxt::ansi {

Mode mode = Mode::disabled;

bool is_tty()
{
    return isatty(STDOUT_FILENO) != 0;
}

void init()
{
    mode = is_tty() ? Mode::enabled : Mode::disabled;
}

namespace {
constexpr std::string_view CSI = "\x1b[";
constexpr std::string_view CSI_DEBUG = "⟨CSI:";
} // namespace

// ============================================================================
// Writer implementation (buffered output)
// ============================================================================

void Writer::csi(std::string_view params, char final_byte)
{
    switch (mode) {
    case Mode::disabled:
        // No ANSI output
        break;
    case Mode::debug:
        std::format_to(
            std::back_inserter(buf_),
            "{}{}{}⟩",
            CSI_DEBUG,
            params,
            final_byte);
        break;
    case Mode::enabled:
        std::format_to(
            std::back_inserter(buf_), "{}{}{}", CSI, params, final_byte);
        break;
    }
}

Writer & Writer::move_to(const ansi_row_t row, const ansi_col_t col)
{
    // ansi_origin is at terminal position -1, so the offset from
    // ansi_origin directly gives us the 1-based ANSI coordinate.
    const auto row_num = (row - ansi_origin_v).count();
    const auto col_num = (col - ansi_origin).count();
    csi(std::format("{};{}", row_num, col_num), 'H');
    return *this;
}

Writer & Writer::move_to(const Pos pos)
{
    return move_to(to_ansi_y(pos), to_ansi_x(pos));
}

Writer & Writer::move_up(const height_t n)
{
    const auto rows = n.count();
    if (rows > 0)
        csi(std::format("{}", rows), 'A');
    return *this;
}

Writer & Writer::move_down(const height_t n)
{
    const auto rows = n.count();
    if (rows > 0)
        csi(std::format("{}", rows), 'B');
    return *this;
}

Writer & Writer::move_right(const width_t n)
{
    const auto cols = n.count();
    if (cols > 0)
        csi(std::format("{}", cols), 'C');
    return *this;
}

Writer & Writer::move_left(const width_t n)
{
    const auto cols = n.count();
    if (cols > 0)
        csi(std::format("{}", cols), 'D');
    return *this;
}

Writer & Writer::move(const Size delta)
{
    move_right(delta.w);
    move_down(delta.h);
    return *this;
}

Writer & Writer::move_to_column(const ansi_col_t col)
{
    const auto col_num = (col - ansi_origin).count();
    csi(std::format("{}", col_num), 'G');
    return *this;
}

Writer & Writer::clear_screen()
{
    csi("2", 'J');
    return *this;
}

Writer & Writer::clear_screen_from_cursor()
{
    csi("0", 'J');
    return *this;
}

Writer & Writer::clear_screen_to_cursor()
{
    csi("1", 'J');
    return *this;
}

Writer & Writer::clear_line()
{
    csi("2", 'K');
    return *this;
}

Writer & Writer::clear_line_from_cursor()
{
    csi("0", 'K');
    return *this;
}

Writer & Writer::clear_line_to_cursor()
{
    csi("1", 'K');
    return *this;
}

Writer & Writer::set_scroll_region(const row_t top, const row_t bottom)
{
    // Convert terminal row_t to 1-based ANSI row via ansi_origin_v
    const auto top_row = (top - ansi_origin_v).count();
    const auto bottom_row = (bottom - ansi_origin_v).count();
    csi(std::format("{};{}", top_row, bottom_row), 'r');
    return *this;
}

Writer & Writer::reset_scroll_region()
{
    csi("", 'r');
    return *this;
}

Writer & Writer::scroll_up(const height_t n)
{
    const auto rows = n.count();
    if (rows > 0)
        csi(std::format("{}", rows), 'S');
    return *this;
}

Writer & Writer::scroll_down(const height_t n)
{
    const auto rows = n.count();
    if (rows > 0)
        csi(std::format("{}", rows), 'T');
    return *this;
}

Writer & Writer::hide_cursor()
{
    csi("?25", 'l');
    return *this;
}

Writer & Writer::show_cursor()
{
    csi("?25", 'h');
    return *this;
}

Writer & Writer::begin_synchronized_update()
{
    csi("?2026", 'h');
    return *this;
}

Writer & Writer::end_synchronized_update()
{
    csi("?2026", 'l');
    return *this;
}

Writer & Writer::save_cursor()
{
    csi("", 's');
    return *this;
}

Writer & Writer::restore_cursor()
{
    csi("", 'u');
    return *this;
}

Writer & Writer::request_cursor_position()
{
    // DSR 6 - Device Status Report (cursor position)
    csi("6", 'n');
    return *this;
}

Writer & Writer::fg(const Rgb8 color)
{
    return fg(color.r, color.g, color.b);
}

Writer & Writer::fg(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    csi(std::format("38;2;{};{};{}", r, g, b), 'm');
    return *this;
}

Writer & Writer::bg(const Rgb8 color)
{
    return bg(color.r, color.g, color.b);
}

Writer & Writer::bg(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    csi(std::format("48;2;{};{};{}", r, g, b), 'm');
    return *this;
}

Writer & Writer::fg(TerminalColor c)
{
    csi(std::format("{}", static_cast<int>(c)), 'm');
    return *this;
}

Writer & Writer::bg(TerminalColor c)
{
    // Background colors are +10 from foreground
    csi(std::format("{}", static_cast<int>(c) + 10), 'm');
    return *this;
}

Writer & Writer::fg_palette(std::uint8_t index)
{
    csi(std::format("38;5;{}", index), 'm');
    return *this;
}

Writer & Writer::bg_palette(std::uint8_t index)
{
    csi(std::format("48;5;{}", index), 'm');
    return *this;
}

Writer & Writer::fg_default()
{
    csi("39", 'm');
    return *this;
}

Writer & Writer::bg_default()
{
    csi("49", 'm');
    return *this;
}

Writer & Writer::style(Emphasis e)
{
    if (has_emphasis(e, Emphasis::bold))
        csi("1", 'm');
    if (has_emphasis(e, Emphasis::faint))
        csi("2", 'm');
    if (has_emphasis(e, Emphasis::italic))
        csi("3", 'm');
    if (has_emphasis(e, Emphasis::underline))
        csi("4", 'm');
    if (has_emphasis(e, Emphasis::blink))
        csi("5", 'm');
    if (has_emphasis(e, Emphasis::reverse))
        csi("7", 'm');
    if (has_emphasis(e, Emphasis::conceal))
        csi("8", 'm');
    if (has_emphasis(e, Emphasis::strikethrough))
        csi("9", 'm');
    return *this;
}

Writer & Writer::reset()
{
    csi("0", 'm');
    return *this;
}

Writer & Writer::bold()
{
    return style(Emphasis::bold);
}

Writer & Writer::dim()
{
    return style(Emphasis::faint);
}

Writer & Writer::italic()
{
    return style(Emphasis::italic);
}

Writer & Writer::underline()
{
    return style(Emphasis::underline);
}

Writer & Writer::reverse()
{
    return style(Emphasis::reverse);
}

Writer & Writer::text(std::string_view str)
{
    buf_.append(str);
    return *this;
}

// ============================================================================
// Standalone functions (immediate output to stdout)
// ============================================================================

namespace {
/// Print a CSI sequence based on current mode
template<typename... Args>
void print_csi(std::format_string<Args...> fmt_str, Args &&... args)
{
    std::string buf;
    switch (mode) {
    case Mode::disabled:
        // No ANSI output
        break;
    case Mode::debug:
        buf.append(CSI_DEBUG);
        std::format_to(
            std::back_inserter(buf), fmt_str, std::forward<Args>(args)...);
        buf.append("⟩");
        std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::cout.flush();
        break;
    case Mode::enabled:
        buf.append(CSI);
        std::format_to(
            std::back_inserter(buf), fmt_str, std::forward<Args>(args)...);
        std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::cout.flush();
        break;
    }
}
} // namespace

void move_to(const ansi_row_t row, const ansi_col_t col)
{
    const auto row_num = (row - ansi_origin_v).count();
    const auto col_num = (col - ansi_origin).count();
    print_csi("{};{}H", row_num, col_num);
}

void move_to(const Pos pos)
{
    move_to(to_ansi_y(pos), to_ansi_x(pos));
}

void clear_screen()
{
    print_csi("2J");
}

void clear_line()
{
    print_csi("2K");
}

void hide_cursor()
{
    print_csi("?25l");
}

void show_cursor()
{
    print_csi("?25h");
}

void begin_synchronized_update()
{
    std::string buf;
    Writer w(buf);
    w.begin_synchronized_update();
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
}

void end_synchronized_update()
{
    std::string buf;
    Writer w(buf);
    w.end_synchronized_update();
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
}

void set_scroll_region(const row_t top, const row_t bottom)
{
    const auto top_row = (top - ansi_origin_v).count();
    const auto bottom_row = (bottom - ansi_origin_v).count();
    print_csi("{};{}r", top_row, bottom_row);
}

void reset_scroll_region()
{
    print_csi("r");
}

void scroll_up(const height_t n)
{
    const auto rows = n.count();
    if (rows > 0)
        print_csi("{}S", rows);
}

void scroll_down(const height_t n)
{
    const auto rows = n.count();
    if (rows > 0)
        print_csi("{}T", rows);
}

std::optional<Pos> query_cursor_position()
{
    if (!is_tty())
        return std::nullopt;

    // Save current terminal settings
    struct termios old_term{};
    struct termios new_term{};
    if (tcgetattr(STDIN_FILENO, &old_term) < 0)
        return std::nullopt;

    // Set raw mode temporarily (disable canonical mode and echo)
    new_term = old_term;
    new_term.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    new_term.c_cc[VMIN] = 0;
    new_term.c_cc[VTIME] = 1; // 100ms timeout per read
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_term) < 0)
        return std::nullopt;

    // Send DSR 6 query
    std::cout << CSI << "6n";
    std::cout.flush();

    // Read response: ESC [ row ; col R
    char buf[32];
    int i = 0;
    bool got_esc = false;
    bool got_csi = false;

    // Use poll to wait for response with timeout
    struct pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    while (i < 31) {
        // Wait up to 100ms for data
        int ret = poll(&pfd, 1, 100);
        if (ret <= 0)
            break; // Timeout or error

        char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
            break;

        if (!got_esc && c == '\x1b') {
            got_esc = true;
            continue;
        }
        if (got_esc && !got_csi && c == '[') {
            got_csi = true;
            continue;
        }
        if (got_csi) {
            if (c == 'R') {
                buf[i] = '\0';
                break;
            }
            buf[i++] = c;
        }
    }

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);

    if (!got_csi || i == 0)
        return std::nullopt;

    // Parse "row;col"
    int row = 0;
    int col = 0;
    if (std::sscanf(buf, "%d;%d", &row, &col) != 2)
        return std::nullopt;

    // Convert from 1-based ANSI to our coordinate system
    // ansi_origin is at -1, so ANSI position 1 = terminal_origin (0)
    return Pos{
        terminal_origin + (col - 1) * ch,
        terminal_origin_v + (row - 1) * ln};
}

TerminalGuard::TerminalGuard()
{
    init();
    hide_cursor();
}

TerminalGuard::~TerminalGuard()
{
    // Get terminal width for ruler
    struct winsize ws{};
    width_t term_width = 80 * ch;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        term_width = ws.ws_col * ch;

    // Save cursor before scroll region reset (which moves to home)
    std::string buf;
    Writer w(buf);
    w.save_cursor();
    w.reset_scroll_region();
    w.restore_cursor();
    w.reset();
    w.show_cursor();
    //  w.text ("\n");
    // w.text ("\n");
    // w.text (tui::hrule_string (term_width));
    // w.text ("\n");
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
}

SynchronizedUpdate::SynchronizedUpdate(bool enabled)
    : enabled_(enabled)
{
    if (enabled_)
        begin_synchronized_update();
}

SynchronizedUpdate::~SynchronizedUpdate()
{
    if (enabled_)
        end_synchronized_update();
}

} // namespace nxt::ansi
