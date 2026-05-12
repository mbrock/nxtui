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
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <string_view>

#include "nxtio/app.hpp"
#include "nxt/ansi.hpp"
#include "nxtio/async-core.hpp"
#include "nxtio/input.hpp"
#include "nxt/raster.hpp"
#include "nxtio/short_id.hpp"
#include "nxt/tui_terminal.hpp"
#include "nxt/units.hpp"

#include <nlohmann/json.hpp>

#ifdef NXT_HAVE_PNG
#  include "nxt/png.hpp"
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

// Streambuf adapter that forwards every byte to the real stdout
// streambuf, the mirrored libvterm, and (when set) a sink callback
// that copies the bytes into the trace.
//
// The real stdout is forwarded verbatim: the kernel TTY discipline
// (with ONLCR on, which is the default for non-raw output) does the
// `\n` → `\r\n` translation on the wire to the real terminal.
//
// The mirrored vterm and the trace sink see the *post-translation*
// stream — we apply ONLCR ourselves before forwarding to them — so
// the live `vt_` (used by `snapshot()`) and the replayed trace both
// match what the real terminal actually rendered. Without this, bare
// `\n` advances the row but not the column in libvterm, and any
// scrollback line writes drift to the right of the previous line.
class TeeStreambuf : public std::streambuf
{
public:
    using Sink = std::function<void(std::string_view)>;

    TeeStreambuf(std::streambuf * real, nxt::vterm::Terminal * vt) noexcept
        : real_(real)
        , vt_(vt)
    {
    }

    void set_sink(Sink sink) noexcept
    {
        sink_ = std::move(sink);
    }

protected:
    int_type overflow(int_type ch) override
    {
        if (ch == traits_type::eof())
            return traits_type::not_eof(ch);
        auto c = traits_type::to_char_type(ch);
        fan_out_translated(std::string_view{&c, 1});
        return real_->sputc(c);
    }

    std::streamsize xsputn(const char * s, std::streamsize n) override
    {
        fan_out_translated(
            std::string_view{s, static_cast<std::size_t>(n)});
        return real_->sputn(s, n);
    }

    int sync() override
    {
        return real_->pubsync();
    }

private:
    // Walk `in` and feed vt_/sink a CRLF-normalized copy. We keep a
    // small running buffer so an unbroken span of bytes that has no
    // `\n` is forwarded as a single chunk (the vt_ writer and sink
    // are happier with batched calls).
    void fan_out_translated(std::string_view in)
    {
        if (in.empty())
            return;
        if (vt_ == nullptr && !sink_) {
            // Still need to update `last_was_cr_` so a `\r` straddling
            // a chunk boundary gets remembered.
            if (!in.empty())
                last_was_cr_ = in.back() == '\r';
            return;
        }

        std::string scratch;
        scratch.reserve(in.size() + 8);
        for (auto c : in) {
            if (c == '\n' && !last_was_cr_)
                scratch.push_back('\r');
            scratch.push_back(c);
            last_was_cr_ = (c == '\r');
        }
        auto view = std::string_view{scratch};
        if (vt_ != nullptr)
            vt_->write(view);
        if (sink_)
            sink_(view);
    }

    std::streambuf * real_;
    nxt::vterm::Terminal * vt_;
    Sink sink_;
    // Tracks the trailing byte of the previous chunk so a `\r` at the
    // end of one write doesn't get double-CR'd if the next write
    // starts with `\n`.
    bool last_was_cr_ = false;
};

std::string make_short_id() noexcept
{
    try {
        return nxt::io::make_short_id();
    } catch (...) {
        // getrandom can fail very early in init; degrade to a fixed
        // placeholder rather than crashing the runtime.
        return "00000000";
    }
}

[[nodiscard]] const char * program_short_name() noexcept
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
    || defined(__NetBSD__)
    const char * prog = getprogname();
#elif defined(__linux__)
    const char * prog = program_invocation_short_name;
#else
    const char * prog = nullptr;
#endif
    if (prog == nullptr || *prog == '\0')
        return "nxt";
    return prog;
}

std::string make_session_tag(std::string_view short_id) noexcept
{
    return std::string{program_short_name()} + "-" + std::string{short_id};
}

[[nodiscard]] std::optional<std::string>
resolve_trace_path(std::string_view run_id) noexcept
{
    // `NXT_TRACE` is the explicit on-switch:
    //   unset / empty   → tracing disabled
    //   `auto`          → traces/<prog>-<run_id>.arrow (mkdir lazily)
    //   anything else   → that exact path
    // Keeping it opt-in avoids littering every `meson test` run with
    // trace files while still making "turn it on" a one-env-var step.
    const char * raw = std::getenv("NXT_TRACE");
    if (raw == nullptr || raw[0] == '\0')
        return std::nullopt;

    std::string value{raw};
    if (value == "auto") {
        std::filesystem::path dir{"traces"};
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return std::nullopt;
        return (dir / (make_session_tag(run_id) + ".arrow")).string();
    }
    return value;
}

[[nodiscard]] bool is_line_blank(std::string_view line) noexcept
{
    for (auto ch : line)
        if (ch != ' ' && ch != '\t' && ch != '\r')
            return false;
    return true;
}

[[nodiscard]] std::string trim_blank_boundary_lines(std::string_view text)
{
    while (!text.empty()) {
        auto end = text.find('\n');
        auto line =
            end == std::string_view::npos ? text : text.substr(0, end);
        if (!is_line_blank(line))
            break;
        if (end == std::string_view::npos)
            return {};
        text.remove_prefix(end + 1);
    }

    while (!text.empty()) {
        auto end = text.size();
        if (text.back() == '\n')
            --end;
        auto begin = text.rfind('\n', end == 0 ? 0 : end - 1);
        begin = begin == std::string_view::npos ? 0 : begin + 1;
        auto line = text.substr(begin, end - begin);
        if (!is_line_blank(line))
            break;
        text = text.substr(0, begin == 0 ? 0 : begin - 1);
    }

    return std::string{text};
}

[[nodiscard]] std::string horizontal_rule(std::size_t width)
{
    std::string out;
    out.reserve(width * std::string_view{"─"}.size());
    for (std::size_t i = 0; i < width; ++i)
        out += "─";
    return out;
}

[[nodiscard]] int
scrollback_bottom_row(const height_t hud_h, const height_t term_h)
{
    if (hud_h > 0 * ln && hud_h < term_h)
        return static_cast<int>((term_h - hud_h - 2 * ln).count());
    return static_cast<int>((term_h - 1 * ln).count());
}

[[nodiscard]] int count_newlines(std::string_view text) noexcept
{
    return static_cast<int>(std::ranges::count(text, '\n'));
}

} // namespace

UIRuntime::UIRuntime()
    : scheduler_(nxt::scheduler::make_unique(nxt::scheduler::options{}))
    , terminal_surface_(isatty(STDOUT_FILENO) != 0)
    , run_id_(make_short_id())
    , root_span_id_(make_short_id())
    , trace_(resolve_trace_path(run_id_), run_id_)
    , screenshot_session_tag_(make_session_tag(run_id_))
{
    signals_.watch(SIGINT, SIGTERM, SIGWINCH);
    (void) refresh_terminal_size();

    // Create compositor with initial terminal size
    compositor_ =
        std::make_unique<TerminalCompositor>(terminal_size(), glyphs_);
    // Share the runtime's stdout mutex so compositor frame flushes
    // serialize against print / println on other coroutine threads.
    compositor_->set_output_mutex(&output_mutex_);

    // Mirror stdout into a headless libvterm so snapshot() can render
    // the whole visible terminal (HUD + scrollback) instead of just
    // the HUD raster the compositor owns. Tee only when we actually
    // have a terminal surface: non-TTY runs have no meaningful screen.
    if (terminal_surface_) {
        auto rows = static_cast<int>(terminal_height().count());
        auto cols = static_cast<int>(terminal_width().count());
        if (rows > 0 && cols > 0) {
            vt_ = std::make_unique<nxt::vterm::Terminal>(rows, cols);
            tee_buf_ = std::make_unique<TeeStreambuf>(
                std::cout.rdbuf(), vt_.get());
            // Sink stdout bytes into the trace too so a replayer can
            // reconstruct the entire visible terminal at any seq —
            // not just the HUD frame buffer.
            static_cast<TeeStreambuf *>(tee_buf_.get())
                ->set_sink([this](std::string_view chunk) {
                    record_tty_bytes(chunk);
                });
            saved_cout_buf_ = std::cout.rdbuf(tee_buf_.get());
        }
    }

    // Open the run's outermost span so every other row in the trace
    // has a valid ancestry. Use an empty parent to mark this as the
    // root.
    if (trace_.enabled()) {
        emit_span_begin(root_span_id_, {}, "runtime");
        record_tty_init();
    }
}

std::string UIRuntime::allocate_span_id() const
{
    return make_short_id();
}

void UIRuntime::emit_span_begin(
    std::string_view span_id,
    std::string_view parent_span_id,
    std::string_view name)
{
    if (!trace_.enabled())
        return;
    nxt::io::arrow::trace_row row;
    row.phase = "span_begin";
    row.event_type = std::string{name};
    row.data = std::string{name};
    row.span_id = std::string{span_id};
    row.parent_span_id = std::string{parent_span_id};
    row.span_name = std::string{name};
    trace_.add(std::move(row));
}

namespace {

// 64-bit FNV-1a: zero deps, good enough as a "did anything change?"
// guard for raster bytes. Collisions would silently drop a frame from
// the trace, but at 64 bits with kilobyte-scale inputs the rate is
// negligible relative to the frequency of UI changes.
constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) noexcept
{
    auto h = fnv_offset;
    for (auto b : bytes) {
        h ^= b;
        h *= fnv_prime;
    }
    return h;
}

std::vector<std::uint8_t> encode_raster_ansi(const Raster & raster)
{
    // Store the frame as the same ANSI byte stream the terminal
    // would receive. Self-contained — replay is just `printf` — and
    // unicode-safe because the renderer already resolves glyph ids
    // through the live `GlyphTable`. Bigger than a packed cell array
    // in the worst case, but most frames compress well because few
    // cells actually change style between adjacent runs.
    auto text = nxt::ansi::render_raster(raster);
    auto bytes = std::vector<std::uint8_t>{};
    bytes.assign(
        reinterpret_cast<const std::uint8_t *>(text.data()),
        reinterpret_cast<const std::uint8_t *>(text.data()) + text.size());
    return bytes;
}

} // namespace

void UIRuntime::record_frame_snapshot(const Raster & back)
{
    if (!trace_.enabled())
        return;

    auto bytes = encode_raster_ansi(back);
    auto hash = fnv1a(std::span<const std::uint8_t>{bytes});
    if (has_last_frame_hash_ && hash == last_frame_hash_)
        return;

    auto cols = static_cast<std::uint64_t>(back.width().count());
    auto rows = static_cast<std::uint64_t>(back.height().count());

    auto payload = nlohmann::json{
        {"cols", cols},
        {"rows", rows},
        {"hash", hash},
        {"bytes", bytes.size()},
    };

    nxt::io::arrow::trace_row row;
    row.phase = "frame";
    row.event_type = "ansi";
    row.data = std::format(
        "{}x{} bytes={} hash={:016x}", cols, rows, bytes.size(), hash);
    row.payload_json = payload.dump();
    row.span_id = root_span_id_;
    row.span_name = "runtime";
    row.frame_seq = next_frame_seq_++;
    row.payload_kind = "ansi";
    row.payload_bin = std::move(bytes);
    trace_.add(std::move(row));
    last_frame_hash_ = hash;
    has_last_frame_hash_ = true;
}

void UIRuntime::record_tty_init()
{
    if (!trace_.enabled())
        return;
    auto cols = static_cast<std::uint64_t>(terminal_width().count());
    auto rows = static_cast<std::uint64_t>(terminal_height().count());
    auto payload = nlohmann::json{
        {"cols", cols},
        {"rows", rows},
        {"surface", has_terminal_surface()},
    };
    nxt::io::arrow::trace_row row;
    row.phase = "tty";
    row.event_type = "init";
    row.data = std::format("{}x{}", cols, rows);
    row.payload_json = payload.dump();
    row.span_id = root_span_id_;
    row.span_name = "runtime";
    trace_.add(std::move(row));
}

void UIRuntime::record_tty_resize()
{
    if (!trace_.enabled())
        return;
    auto cols = static_cast<std::uint64_t>(terminal_width().count());
    auto rows = static_cast<std::uint64_t>(terminal_height().count());
    auto payload = nlohmann::json{
        {"cols", cols},
        {"rows", rows},
    };
    nxt::io::arrow::trace_row row;
    row.phase = "tty";
    row.event_type = "resize";
    row.data = std::format("{}x{}", cols, rows);
    row.payload_json = payload.dump();
    row.span_id = root_span_id_;
    row.span_name = "runtime";
    trace_.add(std::move(row));
}

void UIRuntime::record_tty_bytes(std::string_view bytes)
{
    if (!trace_.enabled() || bytes.empty())
        return;
    nxt::io::arrow::trace_row row;
    row.phase = "tty";
    row.event_type = "bytes";
    // No `data` payload: these rows can come in huge volume and the
    // bytes themselves carry the meaning. Replayer concatenates the
    // binary payloads in seq order.
    row.span_id = root_span_id_;
    row.span_name = "runtime";
    row.payload_kind = "ansi";
    row.payload_bin.assign(
        reinterpret_cast<const std::uint8_t *>(bytes.data()),
        reinterpret_cast<const std::uint8_t *>(bytes.data())
            + bytes.size());
    trace_.add(std::move(row));
}

void UIRuntime::record_input_event(const nxt::input::KeyEvent & event)
{
    if (!trace_.enabled())
        return;

    auto mods = nlohmann::json{
        {"shift", event.mods.shift},
        {"alt", event.mods.alt},
        {"ctrl", event.mods.ctrl},
        {"super", event.mods.super},
        {"hyper", event.mods.hyper},
        {"meta", event.mods.meta},
    };
    auto payload = nlohmann::json{
        {"key", static_cast<int>(event.key)},
        {"type", static_cast<int>(event.type)},
        {"codepoint", event.codepoint},
        {"mods", std::move(mods)},
        {"text", event.text},
    };
    auto dumped = payload.dump();
    nxt::io::arrow::trace_row row;
    row.phase = "input";
    row.event_type = "key";
    row.data = dumped;
    row.payload_json = std::move(dumped);
    row.span_id = root_span_id_;
    row.span_name = "runtime";
    trace_.add(std::move(row));
}

void UIRuntime::emit_span_end(
    std::string_view span_id,
    std::string_view parent_span_id,
    std::string_view name,
    std::string_view status)
{
    if (!trace_.enabled())
        return;
    auto payload = nlohmann::json::object();
    if (!status.empty())
        payload["status"] = std::string{status};
    nxt::io::arrow::trace_row row;
    row.phase = "span_end";
    row.event_type = std::string{name};
    row.data = std::string{status};
    row.payload_json = payload.empty() ? std::string{} : payload.dump();
    row.span_id = std::string{span_id};
    row.parent_span_id = std::string{parent_span_id};
    row.span_name = std::string{name};
    trace_.add(std::move(row));
}

UIRuntime::~UIRuntime()
{
    // Restore the original cout streambuf before vt_ / tee_buf_ get
    // destroyed: any later cout write (including the snapshot summary
    // below) must not go through a stale tee.
    if (saved_cout_buf_ != nullptr) {
        std::cout.rdbuf(saved_cout_buf_);
        saved_cout_buf_ = nullptr;
    }

    // Close the trace before printing any artifact summary so a
    // crash during summary printing still leaves a complete file.
    if (trace_.enabled()) {
        try {
            emit_span_end(root_span_id_, {}, "runtime", "ok");
            trace_.write();
        } catch (...) {
            // Best-effort: never let trace teardown propagate.
        }
    }

    // Fires after the run()-scope TerminalGuard has destructed and
    // restored the terminal, so plain stdout writes land in normal
    // scrollback. These summaries stay out of the live HUD and out of
    // the auto t2s/t5s shots.
    for (const auto & block : post_exit_blocks_)
        std::cout << block;
    if (!post_exit_blocks_.empty())
        std::cout.flush();

    if (!snapshot_paths_.empty()) {
        std::cout << "\nsnapshots:\n";
        for (const auto & path : snapshot_paths_)
            std::cout << "  " << path << '\n';
        std::cout.flush();
    }
    if (trace_.output_path()) {
        std::cout << "trace: " << *trace_.output_path() << '\n';
        std::cout.flush();
    }
}

void UIRuntime::sync_vterm_size() noexcept
{
    if (vt_ == nullptr)
        return;
    auto rows = static_cast<int>(terminal_height().count());
    auto cols = static_cast<int>(terminal_width().count());
    if (rows > 0 && cols > 0)
        vt_->set_size(rows, cols);
}

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
        auto changed = width != old_width || height != old_height;
        if (changed) {
            sync_vterm_size();
            record_tty_resize();
        }
        return changed;
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
    if (!has_terminal_surface())
        return;

    auto & buffer = compositor_->back_buffer();
    buffer.clear();
    auto view = buffer.view();
    auto size = compositor_->size();
    render_fn(view, size);
    maybe_screenshot();
    // Snapshot the freshly-rendered back buffer before present_frame
    // copies it over the front: we want what the user is about to see,
    // not the previous frame.
    record_frame_snapshot(buffer);
    auto guard = std::scoped_lock{output_mutex_};
    flush_output_queue(std::cout);
    compositor_->present_frame(std::cout);
}

void UIRuntime::maybe_screenshot() noexcept
{
#ifdef NXT_HAVE_PNG
    // Trace files (NXT_TRACE=auto) carry raster snapshots for every
    // changed frame, so the timed `img/<tag>-t2s.png` shots are now
    // redundant. Leave them off unless explicitly requested via
    // `NXT_AUTO_SHOTS=1`; the explicit `self.snapshot()` API is
    // unaffected.
    static const bool enabled = [] {
        const char * raw = std::getenv("NXT_AUTO_SHOTS");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();
    if (!enabled)
        return;

    constexpr std::size_t n =
        sizeof(screenshot_milestones) / sizeof(screenshot_milestones[0]);
    if (screenshot_milestone_index_ >= n)
        return;

    auto elapsed = std::chrono::steady_clock::now() - screenshot_start_;
    while (
        screenshot_milestone_index_ < n
        && elapsed
               >= screenshot_milestones[screenshot_milestone_index_].at) {
        capture_screenshot(
            screenshot_milestones[screenshot_milestone_index_].name);
        ++screenshot_milestone_index_;
    }
#endif
}

void UIRuntime::capture_screenshot(std::string_view milestone) noexcept
{
#ifdef NXT_HAVE_PNG
    if (vt_ == nullptr)
        return;
    try {
        std::filesystem::path dir{"img"};
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return;

        auto path = dir
                    / (screenshot_session_tag_ + "-"
                       + std::string{milestone} + ".png");

        // Same vterm-driven render as snapshot(): the auto-shots also
        // need to see HUD + scrollback, not just the HUD raster.
        Size size{terminal_width(), terminal_height()};
        nxt::Raster raster(size, glyphs_);
        auto view = raster.view();
        nxt::tui::render_vterm_screen(view, size, *vt_, {});
        nxt::png::write(raster, path);
    } catch (...) {
        // Best-effort; never let screenshots break the run.
    }
#else
    (void) milestone;
#endif
}

std::string UIRuntime::snapshot()
{
#ifdef NXT_HAVE_PNG
    if (!has_terminal_surface() || vt_ == nullptr)
        return {};
    try {
        std::filesystem::path dir{"img"};
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return {};
        auto path = dir / (make_short_id() + ".png");

        // Render the mirrored vterm screen (HUD + scrollback) into a
        // fresh raster sized to the full terminal, then PNG-encode.
        // This sees every byte that went to stdout, including the
        // tool result cards that print() wrote directly to scrollback.
        Size size{terminal_width(), terminal_height()};
        nxt::Raster raster(size, glyphs_);
        auto view = raster.view();
        nxt::tui::render_vterm_screen(view, size, *vt_, {});
        nxt::png::write(raster, path);
        auto path_str = path.string();
        snapshot_paths_.push_back(path_str);
        return path_str;
    } catch (...) {
        return {};
    }
#else
    return {};
#endif
}

void UIRuntime::update_hud_height(height_t hud_h)
{
    if (!has_terminal_surface())
        return;

    // Keep a logical scrollback cursor row across HUD-height changes. When
    // the HUD grows, the compositor scrolls visible log rows upward, so the
    // cursor follows. When the HUD shrinks, leave the cursor where it was:
    // subsequent output fills newly exposed rows from the top instead of
    // teleporting to the new bottom and leaving a blank pocket.
    if (hud_h != last_hud_height_) {
        auto old_hud_h = last_hud_height_;
        auto term_h = terminal_height();
        if (scrollback_cursor_row_ && old_hud_h > 0 * ln
            && old_hud_h < term_h && hud_h > 0 * ln && hud_h < term_h) {
            auto old_bottom = scrollback_bottom_row(old_hud_h, term_h);
            auto new_bottom = scrollback_bottom_row(hud_h, term_h);
            if (new_bottom < old_bottom)
                *scrollback_cursor_row_ = std::max(
                    0, *scrollback_cursor_row_ - (old_bottom - new_bottom));
            *scrollback_cursor_row_ =
                std::min(*scrollback_cursor_row_, new_bottom);
        } else {
            scrollback_cursor_row_.reset();
        }
        scrollback_cursor_needs_move_ = true;
        last_hud_height_ = hud_h;
    }

    compositor_->set_hud_height(hud_h, terminal_height());
}

void UIRuntime::enqueue_output(QueuedOutput output)
{
    {
        auto guard = std::scoped_lock{output_mutex_};
        output_queue_.push_back(std::move(output));
    }
    signal_damage();
}

void UIRuntime::println(std::string_view line)
{
    if (!has_terminal_surface()) {
        std::cout << line;
        if (line.empty() || line.back() != '\n')
            std::cout << '\n';
        std::cout.flush();
        return;
    }

    enqueue_output(
        QueuedOutput{
            .kind = QueuedOutput::Kind::line,
            .text = std::string{line},
        });
}

void UIRuntime::print_after_exit(std::string text)
{
    auto guard = std::scoped_lock{output_mutex_};
    post_exit_blocks_.push_back(std::move(text));
}

void UIRuntime::print(std::string_view text)
{
    if (!has_terminal_surface()) {
        std::cout << text;
        std::cout.flush();
        return;
    }

    enqueue_output(
        QueuedOutput{
            .kind = QueuedOutput::Kind::text,
            .text = std::string{text},
        });
}

void UIRuntime::print_block(std::string_view text)
{
    if (!has_terminal_surface()) {
        std::cout << text;
        if (text.empty() || text.back() != '\n')
            std::cout << '\n';
        std::cout.flush();
        return;
    }

    enqueue_output(
        QueuedOutput{
            .kind = QueuedOutput::Kind::block,
            .text = std::string{text},
        });
}

void UIRuntime::flush_output_queue(std::ostream & out)
{
    std::vector<QueuedOutput> pending;
    pending.swap(output_queue_);

    for (const auto & output : pending)
        write_output(out, output);
    out.flush();
}

void UIRuntime::write_output(
    std::ostream & out, const QueuedOutput & output)
{
    auto block_text = output.kind == QueuedOutput::Kind::block
                          ? trim_blank_boundary_lines(output.text)
                          : std::string{};

    if (!has_terminal_surface()) {
        if (output.kind == QueuedOutput::Kind::block) {
            if (block_text.empty())
                return;
            out << horizontal_rule(72) << '\n' << block_text;
            if (block_text.back() != '\n')
                out << '\n';
        } else {
            out << output.text;
        }
        if (output.kind == QueuedOutput::Kind::line
            && (output.text.empty() || output.text.back() != '\n'))
            out << '\n';
        return;
    }

    auto hud_h = compositor_->hud_height();
    auto term_h = terminal_height();

    if (hud_h == 0 * ln) {
        if (output.kind == QueuedOutput::Kind::block) {
            if (block_text.empty())
                return;
            out << horizontal_rule(terminal_width().count()) << '\n'
                << block_text;
            if (block_text.back() != '\n')
                out << '\n';
        } else {
            out << output.text;
        }
        if (output.kind == QueuedOutput::Kind::line
            && (output.text.empty() || output.text.back() != '\n'))
            out << '\n';
        return;
    }

    // No scroll region in full-screen mode.
    if (hud_h > 0 * ln && hud_h >= term_h)
        return;

    // Match `compositor::scroll_bottom_for`: keep a 1-row gap between
    // the scrollback writing line and the HUD when there is a HUD.
    auto scroll_bottom = scrollback_bottom_row(hud_h, term_h);

    std::string buf;
    ansi::Writer w(buf);
    if (!scrollback_cursor_row_)
        scrollback_cursor_row_ = scroll_bottom;
    if (scrollback_cursor_needs_move_) {
        w.move_to(
            Pos::at(
                0 * ch,
                static_cast<std::size_t>(*scrollback_cursor_row_) * ln));
        scrollback_cursor_needs_move_ = false;
    }
    w.reset();
    if (output.kind == QueuedOutput::Kind::block) {
        if (block_text.empty())
            return;
        w.text("\n");
        w.text(block_text);
        if (block_text.back() != '\n')
            w.text("\n");
        w.text("\n");
        w.clear_line_from_cursor();
    } else {
        w.text(output.text);
    }
    if (output.kind == QueuedOutput::Kind::line) {
        w.clear_line_from_cursor();
        if (output.text.empty() || output.text.back() != '\n')
            w.text("\n");
    }

    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    auto advance_rows = 0;
    if (output.kind == QueuedOutput::Kind::block) {
        advance_rows = 1 + count_newlines(block_text);
        if (block_text.empty() || block_text.back() != '\n')
            ++advance_rows;
    } else if (output.kind == QueuedOutput::Kind::line) {
        advance_rows = std::max(1, count_newlines(output.text));
    } else {
        advance_rows = count_newlines(output.text);
    }

    if (scrollback_cursor_row_)
        *scrollback_cursor_row_ =
            std::min(scroll_bottom, *scrollback_cursor_row_ + advance_rows);
}

void UIRuntime::cleanup()
{
    auto guard = std::scoped_lock{output_mutex_};
    flush_output_queue(std::cout);
    // Leave the final HUD frame visible after the render loop exits. The
    // terminal guard restores cursor state and scroll margins separately.
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
                println(
                    "CTRL C! CTRL C! CTRL C! CTRL C! CTRL C! CTRL C! CTRL C!");
                request_shutdown();
                break;

            case SIGWINCH:
                if (refresh_terminal_size()) {
                    scrollback_cursor_row_.reset();
                    scrollback_cursor_needs_move_ = true;
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
        record_input_event(event);
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
                    buffer.data(), static_cast<std::size_t>(n)};
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
