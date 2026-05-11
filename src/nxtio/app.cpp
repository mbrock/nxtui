#include <sys/ioctl.h>
#include <unistd.h>
#include <csignal>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

#include "nxtio/app.hpp"
#include "nxt/ansi.hpp"
#include "nxtio/async-core.hpp"
#include "nxtio/input.hpp"
#include "nxt/units.hpp"

#ifdef NXT_HAVE_PNG
#include "nxt/png.hpp"
#endif

#if defined(__linux__)
extern "C" {
extern char * program_invocation_short_name; // glibc
}
#endif

namespace nxt::ui {

namespace {

#ifdef NXT_HAVE_PNG
struct ScreenshotMilestone
{
    const char * name;
    std::chrono::milliseconds at;
};

constexpr ScreenshotMilestone screenshot_milestones[] = {
    {"start", std::chrono::milliseconds{0}},
    {"t2s", std::chrono::seconds{2}},
    {"t5s", std::chrono::seconds{5}},
};
#endif

std::string make_session_tag() noexcept
{
    auto now = std::chrono::system_clock::now();
    auto stamp = std::format("{:%Y%m%dT%H%M%S}",
                             std::chrono::floor<std::chrono::seconds>(now));
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
    || defined(__NetBSD__)
    const char * prog = getprogname();
#elif defined(__linux__)
    const char * prog = program_invocation_short_name;
#else
    const char * prog = nullptr;
#endif
    if (prog == nullptr || *prog == '\0')
        prog = "nxt";
    return std::string{prog} + "-" + stamp;
}

}  // namespace

UIRuntime::UIRuntime()
    : scheduler_(
          nxt::io_scheduler::make_unique(nxt::io_scheduler::options{}))
    , screenshot_session_tag_(make_session_tag())
{
    signals_.watch(SIGINT, SIGTERM, SIGWINCH);
    (void) refresh_terminal_size();

    // Create compositor with initial terminal size
    compositor_ =
        std::make_unique<TerminalCompositor>(terminal_size(), glyphs_);
}

UIRuntime::~UIRuntime() = default;

bool UIRuntime::refresh_terminal_size() noexcept
{
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0
        && ws.ws_row > 0) {
        auto width = ws.ws_col * ch;
        auto height = ws.ws_row * ln;
        auto old_width = terminal_width();
        auto old_height = terminal_height();
        term_width_.store(width, std::memory_order_release);
        term_height_.store(height, std::memory_order_release);
        return width != old_width || height != old_height;
    }
    return false;
}

void UIRuntime::request_shutdown()
{
    if (stop_source_.stop_requested())
        return; // Already shutting down

    // Capture a final screenshot of the last presented frame, but only
    // if at least one frame actually rendered.
    if (screenshot_milestone_index_ > 0)
        capture_screenshot("exit");

    stop_source_.request_stop();
    damage_event_.set();   // Wake present_loop
    SignalPipe::notify(0); // Wake signal_loop
    input_poll_stop_.signal_stop();
}

void UIRuntime::signal_damage()
{
    damage_counter_.fetch_add(1, std::memory_order_acq_rel);
    damage_event_.set();
}

TermSize UIRuntime::terminal_size() const noexcept
{
    return TermSize{terminal_width(), terminal_height()};
}

nxt::width_t UIRuntime::terminal_width() const noexcept
{
    return term_width_.load(std::memory_order_acquire);
}

nxt::height_t UIRuntime::terminal_height() const noexcept
{
    return term_height_.load(std::memory_order_acquire);
}

void UIRuntime::render_impl(
    std::function<void(RasterView &, Size)> render_fn)
{
    auto & buffer = compositor_->back_buffer();
    buffer.clear();
    auto view = buffer.view();
    auto size = compositor_->size();
    render_fn(view, size);
    maybe_screenshot();
    compositor_->present_frame();
}

void UIRuntime::maybe_screenshot() noexcept
{
#ifdef NXT_HAVE_PNG
    constexpr std::size_t n =
        sizeof(screenshot_milestones) / sizeof(screenshot_milestones[0]);
    if (screenshot_milestone_index_ >= n)
        return;

    auto elapsed = std::chrono::steady_clock::now() - screenshot_start_;
    while (screenshot_milestone_index_ < n
           && elapsed >= screenshot_milestones[screenshot_milestone_index_].at) {
        capture_screenshot(
            screenshot_milestones[screenshot_milestone_index_].name);
        ++screenshot_milestone_index_;
    }
#endif
}

void UIRuntime::capture_screenshot(std::string_view milestone) noexcept
{
#ifdef NXT_HAVE_PNG
    try {
        std::filesystem::path dir{"img"};
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return;

        auto path = dir
            / (screenshot_session_tag_ + "-" + std::string{milestone} + ".png");
        nxt::png::write(compositor_->back_buffer(), path);
    } catch (...) {
        // Best-effort; never let screenshots break the run.
    }
#else
    (void) milestone;
#endif
}

void UIRuntime::update_hud_height(height_t hud_h)
{
    compositor_->set_hud_height(hud_h, terminal_height());
}

void UIRuntime::println(std::string_view line)
{
    auto hud_h = compositor_->hud_height();
    auto term_h = terminal_height();

    if (hud_h == 0 * ln) {
        std::cout << line;
        if (line.empty() || line.back() != '\n')
            std::cout << '\n';
        std::cout.flush();
        return;
    }

    // No scroll region in full-screen mode.
    if (hud_h > 0 * ln && hud_h >= term_h)
        return;

    auto scroll_bottom = hud_h > 0 * ln
        ? term_h - hud_h - 1 * ln
        : term_h - 1 * ln;
    auto has_trailing_newline = !line.empty() && line.back() == '\n';

    std::string buf;
    ansi::Writer w(buf);
    w.move_to(Pos::at(0 * ch, scroll_bottom));
    w.reset(); // Avoid HUD styling leaking into log output
    w.text(line);
    w.clear_line_from_cursor();
    if (!has_trailing_newline)
        w.text("\n");

    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
    scrollback_cursor_initialized_ = true;
}

void UIRuntime::print(std::string_view text)
{
    auto hud_h = compositor_->hud_height();
    auto term_h = terminal_height();

    if (hud_h == 0 * ln) {
        std::cout << text;
        std::cout.flush();
        return;
    }

    // No scroll region in full-screen mode.
    if (hud_h > 0 * ln && hud_h >= term_h)
        return;

    auto scroll_bottom = hud_h > 0 * ln
        ? term_h - hud_h - 1 * ln
        : term_h - 1 * ln;

    std::string buf;
    ansi::Writer w(buf);
    if (!scrollback_cursor_initialized_)
        w.move_to(Pos::at(0 * ch, scroll_bottom));
    w.reset();
    w.text(text);

    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
    scrollback_cursor_initialized_ = true;
}

void UIRuntime::cleanup()
{
    auto hud_h = compositor_->hud_height();
    auto term_h = terminal_height();

    // Nothing to clear in no-HUD or full-screen mode
    if (hud_h == 0 * ln || hud_h >= term_h)
        return;

    // Clear HUD region, preserving cursor position
    std::string buf;
    ansi::Writer w(buf);
    w.save_cursor();

    // Clear from HUD area to bottom using raw row numbers to avoid coord
    // issues
    auto term_rows = static_cast<int>(term_h.count());
    auto hud_rows = static_cast<int>(hud_h.count());
    auto start_row = std::max(0, term_rows - hud_rows);
    // Clear rows start_row through term_rows-1 (0-indexed)
    for (int r = start_row; r < term_rows; ++r) {
        w.move_to(Pos::at(0 * ch, r * ln));
        w.clear_line();
    }

    w.restore_cursor();
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
}

TerminalCompositor & UIRuntime::compositor() noexcept
{
    return *compositor_;
}

nxt::task<> UIRuntime::signal_loop()
{
    while (!shutdown_requested()) {
        // Poll the signal pipe for readability
        co_await scheduler_->poll(signals_.read_fd(), nxt::poll_op::read);

        // Drain all pending signals
        while (auto sig = signals_.try_read()) {
            switch (*sig) {
            case 0: // Internal shutdown request (normal completion)
                signal_damage();
                co_return;

            case SIGINT:
            case SIGTERM:
                println("CTRL C! CTRL C! CTRL C! CTRL C! CTRL C! CTRL C! CTRL C!");
                request_shutdown();
                break;

            case SIGWINCH:
                if (refresh_terminal_size()) {
                    scrollback_cursor_initialized_ = false;
                    co_await resize_queue_.push(terminal_size());
                    signal_damage();
                }
                break;

            default:
                break;
            }
        }
    }

    co_return;
}

nxt::task<> UIRuntime::input_loop()
{
    if (!isatty(STDIN_FILENO))
        co_return;

    nxt::input::Parser parser;
    std::array<char, 256> buffer{};
    constexpr auto pending_timeout = std::chrono::milliseconds{25};
    auto publish = [this](nxt::input::KeyEvent event) -> nxt::task<> {
        auto shutdown = event.is_ctrl_c();
        co_await input_queue_.push(std::move(event));
        if (shutdown)
            request_shutdown();
    };

    while (!shutdown_requested()) {
        auto status = co_await scheduler_->poll(
            STDIN_FILENO,
            nxt::poll_op::read,
            parser.has_pending() ? pending_timeout
                                 : std::chrono::milliseconds{0},
            input_poll_stop_.get_token());

        if (status == nxt::poll_status::cancelled)
            break;
        if (status == nxt::poll_status::timeout) {
            for (auto & event : parser.flush())
                co_await publish(std::move(event));
            continue;
        }
        if (status != nxt::poll_status::read)
            co_return;

        while (!shutdown_requested()) {
            auto n = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (n > 0) {
                auto bytes = std::string_view{
                    buffer.data(),
                    static_cast<std::size_t>(n)};
                for (auto & event : parser.feed(bytes))
                    co_await publish(std::move(event));
                continue;
            }

            if (n == 0)
                break;

            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            co_return;
        }
    }

    for (auto & event : parser.flush())
        co_await publish(std::move(event));
    co_await input_queue_.shutdown();

    co_return;
}

} // namespace nxt::ui
