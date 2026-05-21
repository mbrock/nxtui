// shell_scope: spawn an interactive bash inside a transient
// systemd-run --user --scope unit, live-sample its cgroup, and
// render the stats above the embedded terminal.
//
// This is the minimum useful slice of the "swash-host" idea from
// nxtui.org: one supervised session whose resource usage is
// observed alongside its own scrollback. The shell runs in its own
// cgroup v2 scope, so memory.current, cpu.stat, and pids.current
// reflect *only* what you do inside this terminal — including
// anything you launch from it.

#include <nxt/any_layout.hpp>
#include <nxt/raster.hpp>
#include <nxt/slot.hpp>
#include <nxt/tui.hpp>
#include <nxt/units.hpp>
#include <nxtio/app.hpp>
#include <nxtio/async.hpp>
#include <nxtio/input.hpp>
#include <nxtio/process.hpp>
#include <nxtio/subprocess.hpp>
#include <nxtio/tui_subprocess.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nxt::shell_scope {

using namespace nxt::tui;
using namespace nxt::ui;
using namespace std::chrono_literals;

// ============================================================================
// Cgroup data types and helpers (same shape as cgroup_browser; inlined
// here to keep this demo self-contained).
// ============================================================================

struct bytes_t
{
    std::uint64_t v = 0;
};
struct count_t
{
    std::uint64_t v = 0;
};
struct usec_t
{
    std::uint64_t v = 0;
};

inline std::string format_bytes(bytes_t b)
{
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;
    auto d = static_cast<double>(b.v);
    if (d >= gib)
        return std::format("{:.2f}GiB", d / gib);
    if (d >= mib)
        return std::format("{:.1f}MiB", d / mib);
    if (d >= kib)
        return std::format("{:.0f}KiB", d / kib);
    return std::format("{}B", b.v);
}

inline std::string format_percent(double pct)
{
    if (pct < 0.1)
        return "  0%";
    if (pct < 10.0)
        return std::format("{:>3.1f}%", pct);
    return std::format("{:>3.0f}%", pct);
}

struct CgroupSnapshot
{
    std::chrono::steady_clock::time_point sampled;

    // Memory: current, peak, and the breakdown that backs the stacked
    // bar plus a few dynamics fields that surface pressure-like
    // behavior even before PSI kicks in.
    bytes_t memory_current{};
    bytes_t memory_peak{};
    bytes_t memory_anon{};
    bytes_t memory_file{};
    bytes_t memory_slab{};
    bytes_t memory_kernel{};
    bytes_t memory_sock{};
    bytes_t memory_other{};
    count_t pgfault{};
    count_t pgmajfault{};
    count_t ws_refault_anon{};
    count_t ws_refault_file{};
    count_t ws_activate_anon{};
    count_t ws_activate_file{};

    // CPU: cumulative time + throttling enforcement counters.
    usec_t cpu_usage{};
    usec_t cpu_user{};
    usec_t cpu_system{};
    count_t cpu_nr_periods{};
    count_t cpu_nr_throttled{};
    usec_t cpu_throttled{};

    // I/O: aggregated across block devices in io.stat.
    bytes_t io_rbytes{};
    bytes_t io_wbytes{};
    count_t io_rios{};
    count_t io_wios{};

    // Processes.
    count_t pids{};
    count_t pids_peak{};

    // Pressure stall information: percent of time at least one task
    // was stalled waiting for resource, averaged over 10s.
    double psi_mem_some_avg10 = 0.0;
    double psi_cpu_some_avg10 = 0.0;
    double psi_io_some_avg10 = 0.0;
};

inline std::string format_count_si(count_t c)
{
    auto d = static_cast<double>(c.v);
    if (d >= 1e9)
        return std::format("{:.1f}G", d / 1e9);
    if (d >= 1e6)
        return std::format("{:.1f}M", d / 1e6);
    if (d >= 1e3)
        return std::format("{:.1f}k", d / 1e3);
    return std::format("{}", c.v);
}

inline std::string format_time_us(usec_t u)
{
    auto d = static_cast<double>(u.v);
    if (d >= 3.6e9)
        return std::format("{:.1f}h", d / 3.6e9);
    if (d >= 6e7)
        return std::format("{:.1f}min", d / 6e7);
    if (d >= 1e6)
        return std::format("{:.2f}s", d / 1e6);
    if (d >= 1e3)
        return std::format("{:.0f}ms", d / 1e3);
    return std::format("{}µs", u.v);
}

inline std::uint64_t read_uint_file(
    const std::filesystem::path & path)
{
    auto f = std::ifstream{path};
    if (!f.is_open())
        return 0;
    std::uint64_t v = 0;
    f >> v;
    return v;
}

inline std::string
read_text_file(const std::filesystem::path & path)
{
    auto f = std::ifstream{path};
    if (!f.is_open())
        return {};
    std::string out;
    std::string line;
    while (std::getline(f, line)) {
        out += line;
        out += '\n';
    }
    return out;
}

inline std::unordered_map<std::string, std::uint64_t>
parse_kv(std::string_view text)
{
    std::unordered_map<std::string, std::uint64_t> out;
    auto p = text.data();
    auto end = p + text.size();
    while (p < end) {
        auto le = std::find(p, end, '\n');
        auto sp = std::find(p, le, ' ');
        if (sp != le && sp != p) {
            auto key = std::string{p, sp};
            std::uint64_t v = 0;
            for (auto c : std::string_view{sp + 1, le}) {
                if (c < '0' || c > '9')
                    break;
                v = v * 10 + static_cast<std::uint64_t>(c - '0');
            }
            out.emplace(std::move(key), v);
        }
        p = le + 1;
    }
    return out;
}

// Parse `some avg10=X.XX avg60=X.XX avg300=X.XX total=N` and return
// the `some` avg10. The full-line (`full ...`) is harsher and not
// always meaningful for short bursts, so we surface `some`.
inline double parse_psi_some_avg10(std::string_view text)
{
    auto needle = std::string_view{"some avg10="};
    auto pos = text.find(needle);
    if (pos == std::string_view::npos)
        return 0.0;
    pos += needle.size();
    auto end = text.find(' ', pos);
    if (end == std::string_view::npos)
        end = text.size();
    auto numstr = std::string{text.substr(pos, end - pos)};
    try {
        return std::stod(numstr);
    } catch (...) {
        return 0.0;
    }
}

// io.stat has one line per block device:
//   "8:0 rbytes=N wbytes=N rios=N wios=N dbytes=N dios=N"
// We aggregate across devices into a single totals record.
struct IoTotals
{
    bytes_t rbytes;
    bytes_t wbytes;
    count_t rios;
    count_t wios;
};

inline IoTotals parse_io_stat(std::string_view text)
{
    IoTotals t;
    auto p = text.data();
    auto end = p + text.size();
    while (p < end) {
        auto le = std::find(p, end, '\n');
        auto line = std::string_view{p, le};
        auto extract = [&](std::string_view key) -> std::uint64_t {
            auto k = line.find(key);
            if (k == std::string_view::npos)
                return 0;
            k += key.size();
            std::uint64_t v = 0;
            while (k < line.size() && line[k] >= '0'
                   && line[k] <= '9') {
                v = v * 10 + static_cast<std::uint64_t>(
                                 line[k++] - '0');
            }
            return v;
        };
        t.rbytes.v += extract("rbytes=");
        t.wbytes.v += extract("wbytes=");
        t.rios.v += extract("rios=");
        t.wios.v += extract("wios=");
        p = le + 1;
    }
    return t;
}

inline CgroupSnapshot
read_snapshot(const std::filesystem::path & dir)
{
    CgroupSnapshot s;
    s.sampled = std::chrono::steady_clock::now();
    s.memory_current =
        bytes_t{read_uint_file(dir / "memory.current")};
    s.memory_peak =
        bytes_t{read_uint_file(dir / "memory.peak")};
    s.pids = count_t{read_uint_file(dir / "pids.current")};
    s.pids_peak = count_t{read_uint_file(dir / "pids.peak")};

    if (auto mem = read_text_file(dir / "memory.stat");
        !mem.empty()) {
        auto m = parse_kv(mem);
        auto lookup = [&](std::string_view k) {
            auto it = m.find(std::string{k});
            return it == m.end() ? std::uint64_t{0}
                                 : it->second;
        };
        s.memory_anon = bytes_t{lookup("anon")};
        s.memory_file = bytes_t{lookup("file")};
        s.memory_slab = bytes_t{lookup("slab")};
        s.memory_kernel = bytes_t{lookup("kernel")};
        s.memory_sock = bytes_t{lookup("sock")};
        auto sum = s.memory_anon.v + s.memory_file.v
                   + s.memory_slab.v + s.memory_kernel.v
                   + s.memory_sock.v;
        s.memory_other = bytes_t{
            s.memory_current.v > sum ? s.memory_current.v - sum
                                     : 0};
        s.pgfault = count_t{lookup("pgfault")};
        s.pgmajfault = count_t{lookup("pgmajfault")};
        s.ws_refault_anon =
            count_t{lookup("workingset_refault_anon")};
        s.ws_refault_file =
            count_t{lookup("workingset_refault_file")};
        s.ws_activate_anon =
            count_t{lookup("workingset_activate_anon")};
        s.ws_activate_file =
            count_t{lookup("workingset_activate_file")};
    }
    if (auto cpu = read_text_file(dir / "cpu.stat");
        !cpu.empty()) {
        auto m = parse_kv(cpu);
        auto lookup = [&](std::string_view k) {
            auto it = m.find(std::string{k});
            return it == m.end() ? std::uint64_t{0}
                                 : it->second;
        };
        s.cpu_usage = usec_t{lookup("usage_usec")};
        s.cpu_user = usec_t{lookup("user_usec")};
        s.cpu_system = usec_t{lookup("system_usec")};
        s.cpu_nr_periods = count_t{lookup("nr_periods")};
        s.cpu_nr_throttled =
            count_t{lookup("nr_throttled")};
        s.cpu_throttled = usec_t{lookup("throttled_usec")};
    }
    if (auto io = read_text_file(dir / "io.stat"); !io.empty()) {
        auto t = parse_io_stat(io);
        s.io_rbytes = t.rbytes;
        s.io_wbytes = t.wbytes;
        s.io_rios = t.rios;
        s.io_wios = t.wios;
    }
    if (auto p = read_text_file(dir / "memory.pressure");
        !p.empty())
        s.psi_mem_some_avg10 = parse_psi_some_avg10(p);
    if (auto p = read_text_file(dir / "cpu.pressure"); !p.empty())
        s.psi_cpu_some_avg10 = parse_psi_some_avg10(p);
    if (auto p = read_text_file(dir / "io.pressure"); !p.empty())
        s.psi_io_some_avg10 = parse_psi_some_avg10(p);
    return s;
}

// ============================================================================
// Cgroup discovery from PID. On cgroup v2 a process has exactly one
// line in /proc/PID/cgroup, of the form `0::/path/below/root`.
// ============================================================================

inline std::optional<std::filesystem::path>
read_cgroup_path(pid_t pid)
{
    auto cg = read_text_file(std::format("/proc/{}/cgroup", pid));
    if (cg.empty())
        return std::nullopt;
    auto sep = cg.find("::");
    if (sep == std::string::npos)
        return std::nullopt;
    auto path_start = sep + 2;
    auto path_end = cg.find('\n', path_start);
    if (path_end == std::string::npos)
        path_end = cg.size();
    auto path = std::string_view{
        cg.data() + path_start, path_end - path_start};
    if (path.empty() || path[0] != '/')
        return std::nullopt;
    return std::filesystem::path{"/sys/fs/cgroup"}
           / std::filesystem::path{
               std::string{path.substr(1)}};
}

// systemd-run runs first, then exec's the child; there's a small
// window where /proc/PID/cgroup still points at the parent's
// cgroup. Poll until it lands inside `/swash-*.scope`.
inline nxt::task<std::optional<std::filesystem::path>>
wait_for_scope(
    const yard & self,
    pid_t pid,
    std::string_view unit_name)
{
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (self.cancelled())
            co_return std::nullopt;
        auto p = read_cgroup_path(pid);
        if (p && p->string().find(unit_name) != std::string::npos)
            co_return p;
        co_await self.sleep(50ms);
    }
    co_return read_cgroup_path(pid);
}

// ============================================================================
// Stacked bar painter (same trick as cgroup_browser).
// ============================================================================

constexpr Rgba8 col_anon{220, 130, 80};
constexpr Rgba8 col_file{120, 180, 220};
constexpr Rgba8 col_slab{160, 200, 110};
constexpr Rgba8 col_kernel{180, 140, 220};
constexpr Rgba8 col_sock{230, 200, 90};
constexpr Rgba8 col_other{140, 150, 170};
constexpr Rgba8 col_track{32, 36, 46};

struct Segment
{
    double fraction = 0.0;
    Rgba8 color = Rgba8::black();
};

inline void paint_stacked_bar(
    RasterView & raster,
    nxt::Pos top,
    nxt::width_t width,
    Rgba8 track,
    std::span<const Segment> segments,
    std::vector<std::uint8_t> & is_bar_out)
{
    auto w = static_cast<int>(width.count());
    is_bar_out.assign(static_cast<std::size_t>(std::max(0, w)), 0);
    for (int i = 0; i < w; ++i) {
        auto pos = top + i * nxt::ch;
        raster.set_bg(pos, track);
        raster.set_fg(pos, track);
        raster.write_text(pos, " ");
    }
    double acc = 0.0;
    for (const auto & seg : segments) {
        if (seg.fraction <= 0.0)
            continue;
        double s = acc * w;
        double e = (acc + seg.fraction) * w;
        acc += seg.fraction;
        int i0 = std::clamp(
            static_cast<int>(std::floor(s)), 0, w);
        int i1 = std::clamp(
            static_cast<int>(std::ceil(e)), 0, w);
        for (int i = i0; i < i1; ++i) {
            auto pos = top + i * nxt::ch;
            raster.set_bg(pos, seg.color);
            raster.set_fg(pos, seg.color);
            is_bar_out[static_cast<std::size_t>(i)] = 1;
        }
    }
}

inline std::string
sparkline(std::span<const double> values, std::size_t cells)
{
    static constexpr std::array<std::string_view, 9> blocks = {
        " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█",
    };
    if (values.empty() || cells == 0)
        return std::string(cells, ' ');
    double lo = values.front();
    double hi = values.front();
    for (auto v : values) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (hi - lo < 1e-9)
        hi = lo + 1.0;
    std::string out;
    auto take = std::min(values.size(), cells);
    auto pad = cells - take;
    for (std::size_t i = 0; i < pad; ++i)
        out += " ";
    auto offset = values.size() - take;
    for (std::size_t i = 0; i < take; ++i) {
        auto frac = (values[offset + i] - lo) / (hi - lo);
        auto idx = static_cast<std::size_t>(
            std::round(std::clamp(frac, 0.0, 1.0) * 8.0));
        out += blocks[idx];
    }
    return out;
}

// ============================================================================
// Session context — owned by shared_ptr, lives past root's stack
// because the published layout's leaf lambdas capture a copy.
// ============================================================================

struct SessionCtx
{
    std::shared_ptr<nxt::subprocess::PtySession> session;
    std::string unit_name;
    std::filesystem::path cgroup_path;
    std::deque<CgroupSnapshot> snaps;
    nxt::vterm::Terminal fallback{1, 1};

    static constexpr std::size_t spark_history = 32;

    void push(CgroupSnapshot s)
    {
        snaps.push_back(std::move(s));
        while (snaps.size() > spark_history)
            snaps.pop_front();
    }

    bool ready() const
    {
        return !snaps.empty();
    }

    double cpu_percent() const
    {
        if (snaps.size() < 2)
            return 0.0;
        const auto & a = snaps[snaps.size() - 2];
        const auto & b = snaps.back();
        auto dt = std::chrono::duration_cast<
                      std::chrono::microseconds>(
                      b.sampled - a.sampled)
                      .count();
        if (dt <= 0)
            return 0.0;
        auto dcpu =
            static_cast<std::int64_t>(b.cpu_usage.v)
            - static_cast<std::int64_t>(a.cpu_usage.v);
        if (dcpu < 0)
            dcpu = 0;
        return 100.0 * static_cast<double>(dcpu)
               / static_cast<double>(dt);
    }
};

// ============================================================================
// Coroutines
// ============================================================================

inline nxt::task<>
cgroup_sampler(yard & self, std::shared_ptr<SessionCtx> ctx)
{
    while (!self.cancelled()) {
        if (!ctx->cgroup_path.empty()) {
            ctx->push(read_snapshot(ctx->cgroup_path));
            self.runtime().signal_damage();
        }
        co_await self.sleep(60ms);
    }
}

inline nxt::task<>
pty_reader(yard & self, std::shared_ptr<SessionCtx> ctx)
{
    auto & rt = self.runtime();
    auto status = co_await ctx->session->read_loop(
        rt.scheduler(),
        rt.get_stop_token(),
        [&rt] { rt.signal_damage(); });
    self.println(std::format(
        "shell exited: {}", status.describe()));
    rt.request_shutdown();
}

inline nxt::task<>
input_forwarder(yard & self, std::shared_ptr<SessionCtx> ctx)
{
    auto & rt = self.runtime();
    while (!self.cancelled()) {
        auto event = co_await self.next_input();
        if (!event)
            co_return;

        // Don't forward release events for non-character keys; the
        // shell doesn't care, and forwarding can confuse some
        // applications' Kitty keyboard protocol handling.
        if (event->type == nxt::input::EventType::release)
            continue;

        auto encoded = ctx->session->encode_key(*event);
        if (encoded.empty())
            continue;
        co_await ctx->session->write_all(
            rt.scheduler(),
            std::move(encoded),
            rt.get_stop_token());
    }
}

// ============================================================================
// Header rendering: 3 lines —
//   line 1: unit name + cgroup path
//   line 2: stacked memory bar with name+totals overlay
//   line 3: sparklines (mem) + (cpu) with numeric labels
// ============================================================================

// Helpers for compositing the drawer rows.

inline void fill_row_bg(
    RasterView & r,
    nxt::Pos top,
    nxt::width_t w,
    Rgba8 bg,
    Rgba8 fg)
{
    auto sz = nxt::Size{w, 1 * nxt::ln};
    auto sub = subraster(r, top, sz);
    std::ranges::fill(sub.glyphs(), 32);
    std::ranges::fill(sub.bgs(), bg);
    std::ranges::fill(sub.fgs(), fg);
    std::ranges::fill(sub.ems(), DEFAULT_EMPHASIS);
}

inline void write_colored(
    RasterView & r,
    nxt::Pos pos,
    std::string_view text,
    Rgba8 fg)
{
    r.write_text(pos, std::string{text});
    for (std::size_t j = 0; j < text.size(); ++j)
        r.set_fg(pos + static_cast<int>(j) * nxt::ch, fg);
}

// A small inline 0..100% gauge used for PSI averages.
//   width = number of cells; pct ∈ [0, 100].
inline void paint_meter(
    RasterView & r,
    nxt::Pos top,
    int cells,
    double pct,
    Rgba8 bar,
    Rgba8 track)
{
    static constexpr std::array<std::string_view, 9> blocks = {
        " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█",
    };
    auto frac = std::clamp(pct / 100.0, 0.0, 1.0);
    double fill = frac * cells;
    int full = static_cast<int>(std::floor(fill));
    int part_idx = std::clamp(
        static_cast<int>(std::round((fill - full) * 8.0)),
        0,
        8);
    for (int i = 0; i < cells; ++i) {
        auto pos = top + i * nxt::ch;
        r.set_bg(pos, track);
        r.set_fg(pos, bar);
        if (i < full)
            r.write_text(pos, std::string{blocks[8]});
        else if (i == full && part_idx > 0)
            r.write_text(
                pos, std::string{blocks[part_idx]});
        else
            r.write_text(pos, " ");
    }
}

// Color palette for accent labels.
constexpr Rgba8 fg_text{215, 220, 230};
constexpr Rgba8 fg_dim{135, 145, 165};
constexpr Rgba8 fg_label{120, 180, 220};

inline auto title_row(std::shared_ptr<const SessionCtx> ctx)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx](RasterView & r, nxt::Size sz) {
            fill_row_bg(
                r,
                nxt::Pos::origin(),
                sz.w,
                Rgba8{22, 28, 40},
                fg_text);
            auto pid_str =
                std::format("{}", ctx->session->child_pid());
            auto cg_str = ctx->cgroup_path.empty()
                              ? std::string{"(no scope yet)"}
                              : ctx->cgroup_path.string();
            auto line = std::format(
                " {}  pid {}  ·  {}",
                ctx->unit_name,
                pid_str,
                cg_str);
            auto w = static_cast<int>(sz.w.count());
            if (static_cast<int>(line.size()) > w)
                line = line.substr(0, w);
            r.write_text(nxt::Pos::origin(), line);
        });
}

inline auto memory_bar_row(std::shared_ptr<const SessionCtx> ctx)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx](RasterView & r, nxt::Size sz) {
            auto w = static_cast<int>(sz.w.count());
            auto top = nxt::Pos::origin();
            std::vector<std::uint8_t> is_bar(
                static_cast<std::size_t>(w), 0);
            if (ctx->ready()
                && ctx->snaps.back().memory_current.v > 0) {
                const auto & cur = ctx->snaps.back();
                auto total = static_cast<double>(
                    cur.memory_current.v);
                auto frac = [&](bytes_t b) {
                    return static_cast<double>(b.v) / total;
                };
                std::array<Segment, 6> segs = {
                    Segment{frac(cur.memory_anon), col_anon},
                    Segment{frac(cur.memory_file), col_file},
                    Segment{frac(cur.memory_slab), col_slab},
                    Segment{frac(cur.memory_kernel),
                            col_kernel},
                    Segment{frac(cur.memory_sock), col_sock},
                    Segment{frac(cur.memory_other), col_other},
                };
                paint_stacked_bar(
                    r,
                    top,
                    sz.w,
                    col_track,
                    std::span<const Segment>{segs},
                    is_bar);
            } else {
                fill_row_bg(
                    r, top, sz.w, col_track, fg_text);
            }
            auto pick_fg = [&](int col) {
                if (col < 0 || col >= w)
                    return fg_text;
                return is_bar[
                               static_cast<std::size_t>(col)]
                           ? Rgba8{10, 14, 22}
                           : fg_text;
            };
            auto label = std::string{" memory"};
            r.write_text(top, label);
            for (int j = 0;
                 j < static_cast<int>(label.size()) && j < w;
                 ++j)
                r.set_fg(top + j * nxt::ch, pick_fg(j));
            if (ctx->ready()) {
                const auto & cur = ctx->snaps.back();
                auto tail = std::format(
                    " {:>10}  peak {}  ·  {} pids ",
                    format_bytes(cur.memory_current),
                    format_bytes(cur.memory_peak),
                    cur.pids.v);
                auto col0 =
                    w - static_cast<int>(tail.size());
                if (col0 < 0)
                    col0 = 0;
                r.write_text(top + col0 * nxt::ch, tail);
                for (int j = 0;
                     j < static_cast<int>(tail.size())
                     && col0 + j < w;
                     ++j)
                    r.set_fg(
                        top + (col0 + j) * nxt::ch,
                        pick_fg(col0 + j));
            }
        });
}

inline auto memory_detail_row(
    std::shared_ptr<const SessionCtx> ctx)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx](RasterView & r, nxt::Size sz) {
            fill_row_bg(
                r,
                nxt::Pos::origin(),
                sz.w,
                Rgba8{14, 16, 22},
                fg_text);

            // Color-keyed legend matching the bar above + dynamics
            // (page faults, working-set refaults).
            auto cursor = nxt::Pos::origin() + 1 * nxt::ch;
            auto put_swatch =
                [&](Rgba8 color, std::string_view label) {
                    write_colored(r, cursor, "▆", color);
                    cursor += 1 * nxt::ch;
                    auto txt = std::format(" {} ", label);
                    write_colored(r, cursor, txt, fg_dim);
                    cursor += static_cast<int>(txt.size())
                              * nxt::ch;
                };
            put_swatch(col_anon, "anon");
            put_swatch(col_file, "file");
            put_swatch(col_slab, "slab");
            put_swatch(col_kernel, "kernel");
            put_swatch(col_sock, "sock");

            if (!ctx->ready())
                return;
            const auto & cur = ctx->snaps.back();
            auto faults_str = std::format(
                "  faults {}  major {}  refaults file {} anon {}",
                format_count_si(cur.pgfault),
                format_count_si(cur.pgmajfault),
                format_count_si(cur.ws_refault_file),
                format_count_si(cur.ws_refault_anon));
            auto col0 = static_cast<int>(sz.w.count())
                        - static_cast<int>(faults_str.size())
                        - 1;
            if (col0 < cursor.col())
                col0 = static_cast<int>(cursor.col()) + 1;
            write_colored(
                r,
                nxt::Pos{
                    nxt::Pos::origin().x + col0 * nxt::ch,
                    nxt::Pos::origin().y,
                },
                faults_str,
                fg_dim);
        });
}

inline auto cpu_row(std::shared_ptr<const SessionCtx> ctx)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx](RasterView & r, nxt::Size sz) {
            fill_row_bg(
                r,
                nxt::Pos::origin(),
                sz.w,
                Rgba8{14, 16, 22},
                fg_text);
            auto w = static_cast<int>(sz.w.count());

            // CPU sparkline = recent per-tick rate.
            std::vector<double> cpu_hist;
            cpu_hist.reserve(ctx->snaps.size());
            for (std::size_t i = 1; i < ctx->snaps.size(); ++i) {
                const auto & a = ctx->snaps[i - 1];
                const auto & b = ctx->snaps[i];
                auto dt = std::chrono::duration_cast<
                              std::chrono::microseconds>(
                              b.sampled - a.sampled)
                              .count();
                auto dcpu =
                    static_cast<std::int64_t>(b.cpu_usage.v)
                    - static_cast<std::int64_t>(a.cpu_usage.v);
                cpu_hist.push_back(
                    dt > 0
                        ? 100.0
                              * static_cast<double>(
                                  std::max<std::int64_t>(
                                      0, dcpu))
                              / static_cast<double>(dt)
                        : 0.0);
            }
            auto spark = sparkline(cpu_hist, 16);

            auto origin = nxt::Pos::origin();
            write_colored(r, origin + 1 * nxt::ch, "cpu", fg_label);
            write_colored(
                r,
                origin + 5 * nxt::ch,
                spark,
                Rgba8{160, 200, 110});

            if (!ctx->ready())
                return;
            const auto & cur = ctx->snaps.back();
            auto user_pct = cur.cpu_usage.v == 0
                                ? 0
                                : 100 * cur.cpu_user.v
                                      / cur.cpu_usage.v;
            auto sys_pct = cur.cpu_usage.v == 0
                               ? 0
                               : 100 * cur.cpu_system.v
                                     / cur.cpu_usage.v;
            auto tail = std::format(
                "  total {}   user {} ({}%)   sys {} ({}%)   "
                "throttled {} / {} periods",
                format_time_us(cur.cpu_usage),
                format_time_us(cur.cpu_user),
                user_pct,
                format_time_us(cur.cpu_system),
                sys_pct,
                format_time_us(cur.cpu_throttled),
                cur.cpu_nr_throttled.v);
            // Write tail right after the sparkline.
            auto col = 5 + 16;
            if (col < w)
                write_colored(
                    r, origin + col * nxt::ch, tail, fg_dim);
        });
}

inline auto io_row(std::shared_ptr<const SessionCtx> ctx)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx](RasterView & r, nxt::Size sz) {
            fill_row_bg(
                r,
                nxt::Pos::origin(),
                sz.w,
                Rgba8{14, 16, 22},
                fg_text);
            auto origin = nxt::Pos::origin();
            write_colored(
                r, origin + 1 * nxt::ch, "i/o", fg_label);

            // Read/write throughput sparklines as bytes/sec.
            std::vector<double> read_hist;
            std::vector<double> write_hist;
            for (std::size_t i = 1; i < ctx->snaps.size(); ++i) {
                const auto & a = ctx->snaps[i - 1];
                const auto & b = ctx->snaps[i];
                auto dt = std::chrono::duration_cast<
                              std::chrono::microseconds>(
                              b.sampled - a.sampled)
                              .count();
                auto dr = static_cast<std::int64_t>(
                              b.io_rbytes.v)
                          - static_cast<std::int64_t>(
                              a.io_rbytes.v);
                auto dw = static_cast<std::int64_t>(
                              b.io_wbytes.v)
                          - static_cast<std::int64_t>(
                              a.io_wbytes.v);
                read_hist.push_back(
                    dt > 0
                        ? 1e6 * static_cast<double>(
                                    std::max<std::int64_t>(
                                        0, dr))
                              / static_cast<double>(dt)
                        : 0.0);
                write_hist.push_back(
                    dt > 0
                        ? 1e6 * static_cast<double>(
                                    std::max<std::int64_t>(
                                        0, dw))
                              / static_cast<double>(dt)
                        : 0.0);
            }
            auto r_spark = sparkline(read_hist, 8);
            auto w_spark = sparkline(write_hist, 8);
            write_colored(
                r, origin + 5 * nxt::ch, r_spark, col_file);
            write_colored(
                r,
                origin + 14 * nxt::ch,
                w_spark,
                Rgba8{220, 150, 100});

            if (!ctx->ready())
                return;
            const auto & cur = ctx->snaps.back();
            auto tail = std::format(
                "  read {}  write {}  iops {}r / {}w",
                format_bytes(cur.io_rbytes),
                format_bytes(cur.io_wbytes),
                format_count_si(cur.io_rios),
                format_count_si(cur.io_wios));
            auto col = 22;
            auto w = static_cast<int>(sz.w.count());
            if (col < w)
                write_colored(
                    r, origin + col * nxt::ch, tail, fg_dim);
        });
}

inline auto pressure_row(
    std::shared_ptr<const SessionCtx> ctx)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx](RasterView & r, nxt::Size sz) {
            fill_row_bg(
                r,
                nxt::Pos::origin(),
                sz.w,
                Rgba8{14, 16, 22},
                fg_text);
            auto origin = nxt::Pos::origin();
            write_colored(
                r,
                origin + 1 * nxt::ch,
                "pressure (10s avg %)",
                fg_label);

            if (!ctx->ready())
                return;
            const auto & cur = ctx->snaps.back();
            struct PsiBar
            {
                std::string_view label;
                double pct;
                Rgba8 color;
            };
            std::array<PsiBar, 3> bars = {
                PsiBar{"mem", cur.psi_mem_some_avg10,
                       col_anon},
                PsiBar{"cpu", cur.psi_cpu_some_avg10,
                       Rgba8{160, 200, 110}},
                PsiBar{"io", cur.psi_io_some_avg10,
                       col_file},
            };
            auto col = 22;
            for (const auto & b : bars) {
                auto label =
                    std::format("  {} ", b.label);
                write_colored(
                    r, origin + col * nxt::ch, label, fg_dim);
                col += static_cast<int>(label.size());
                paint_meter(
                    r,
                    origin + col * nxt::ch,
                    10,
                    b.pct,
                    b.color,
                    col_track);
                col += 10;
                auto val_str =
                    std::format(" {:>5.2f}%", b.pct);
                write_colored(
                    r,
                    origin + col * nxt::ch,
                    val_str,
                    b.pct > 1.0 ? b.color : fg_dim);
                col += static_cast<int>(val_str.size());
            }
        });
}

inline auto header_layout(
    std::shared_ptr<const SessionCtx> ctx)
{
    // Six fixed rows. Each is its own focused topic so a glance at
    // any line tells you about one aspect of resource use.
    return column(
        title_row(ctx),
        memory_bar_row(ctx),
        memory_detail_row(ctx),
        cpu_row(ctx),
        io_row(ctx),
        pressure_row(ctx));
}

// ============================================================================
// Status footer
// ============================================================================

inline auto status_bar(std::shared_ptr<const SessionCtx> ctx)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx](RasterView & r, nxt::Size sz) {
            (void) sz;
            std::ranges::fill(r.glyphs(), 32);
            std::ranges::fill(r.fgs(), Rgba8{220, 220, 230});
            std::ranges::fill(r.bgs(), Rgba8{30, 34, 40});
            std::ranges::fill(r.ems(), DEFAULT_EMPHASIS);
            auto line = std::format(
                " shell_scope   exit shell (Ctrl-D / `exit`) to "
                "quit   samples {}",
                ctx->snaps.size());
            r.write_text(nxt::Pos::origin(), line);
        });
}

// ============================================================================
// Fullscreen wrapper: header on top, terminal in middle, status at
// bottom. Reports flex>0 so the runtime treats it as fullscreen.
// ============================================================================

template<Layout Head, Layout Body, Layout Foot>
struct Screen
{
    Head head;
    Body body;
    Foot foot;

    WidthHint width_hint() const
    {
        return WidthHint::grow();
    }
    HeightHint height_hint() const
    {
        return HeightHint{
            head.height_hint().min
                + body.height_hint().min
                + foot.height_hint().min,
            1.0 * nxt::one,
        };
    }

    void render(RasterView & raster, nxt::Size size) const
    {
        if (size.h.count() == 0)
            return;
        auto head_h = std::min(
            head.height_hint().min, size.h);
        auto foot_h = std::min(
            foot.height_hint().min, size.h - head_h);
        auto body_h = size.h - head_h - foot_h;
        auto origin = nxt::Pos::origin();
        if (head_h.count() > 0) {
            auto sz = nxt::Size{size.w, head_h};
            auto sub = subraster(raster, origin, sz);
            head.render(sub, sz);
        }
        if (body_h.count() > 0) {
            auto sz = nxt::Size{size.w, body_h};
            auto sub = subraster(
                raster, origin + head_h, sz);
            body.render(sub, sz);
        }
        if (foot_h.count() > 0) {
            auto sz = nxt::Size{size.w, foot_h};
            auto sub = subraster(
                raster, origin + head_h + body_h, sz);
            foot.render(sub, sz);
        }
    }
};

template<Layout H, Layout B, Layout F>
auto screen(H && h, B && b, F && f)
{
    return Screen<std::decay_t<H>, std::decay_t<B>,
                  std::decay_t<F>>{
        std::forward<H>(h),
        std::forward<B>(b),
        std::forward<F>(f),
    };
}

// ============================================================================
// Root body: spawn shell-inside-systemd-run-scope, discover its
// cgroup, spawn coroutines, draw layout, wait for shell exit.
// ============================================================================

inline std::string make_unit_name()
{
    auto now = std::chrono::system_clock::now();
    auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                     now.time_since_epoch())
                     .count();
    std::mt19937_64 rng{static_cast<std::uint64_t>(stamp)};
    auto suffix = rng() & 0xFFFFFF;
    return std::format("swash-{}-{:06x}", getpid(), suffix);
}

inline nxt::task<> root(yard & self)
{
    auto & rt = self.runtime();

    // Spawn shell via `systemd-run --user --scope ... bash`.
    auto unit_name = make_unit_name();
    auto term_size = nxt::Size{
        rt.terminal_width(),
        std::max<height_t>(
            10 * nxt::ln,
            rt.terminal_height() - 5 * nxt::ln),
    };
    nxt::subprocess::SpawnOptions opts;
    opts.argv = {
        "systemd-run",
        "--user",
        "--scope",
        "--quiet",
        std::format("--unit={}", unit_name),
        "bash",
    };
    opts.size = term_size;

    std::shared_ptr<nxt::subprocess::PtySession> session;
    try {
        session = std::make_shared<
            nxt::subprocess::PtySession>(
            nxt::subprocess::PtySession::spawn(opts));
    } catch (const std::exception & e) {
        self.println(std::format(
            "failed to spawn shell: {}", e.what()));
        co_return;
    }

    auto ctx = std::make_shared<SessionCtx>();
    ctx->session = session;
    ctx->unit_name = unit_name;

    self.draw(screen(
        header_layout(ctx),
        pty_screen(*session, bg(Rgba8{12, 14, 18})),
        status_bar(ctx)));

    // Wait for systemd-run to place the child in its scope.
    auto found = co_await wait_for_scope(
        self, session->child_pid(), unit_name);
    if (found) {
        ctx->cgroup_path = std::move(*found);
        self.println(std::format(
            "cgroup: {}", ctx->cgroup_path.string()));
    } else {
        self.println(
            "warning: could not locate scope cgroup; "
            "stats panel will stay empty");
    }

    // Spawn the three persistent coroutines: PTY reader, input
    // forwarder, cgroup sampler.
    auto reader_h = self.spawn(
        [ctx](yard & s) -> nxt::task<> {
            return pty_reader(s, ctx);
        });
    auto input_h = self.spawn(
        [ctx](yard & s) -> nxt::task<> {
            return input_forwarder(s, ctx);
        });
    auto sampler_h = self.spawn(
        [ctx](yard & s) -> nxt::task<> {
            return cgroup_sampler(s, ctx);
        });
    (void) reader_h;
    (void) input_h;
    (void) sampler_h;

    // Block until the runtime is shut down (shell exit triggers
    // request_shutdown inside pty_reader).
    while (!self.cancelled()
           && !rt.shutdown_requested()) {
        co_await self.sleep(200ms);
    }

    self.cancel();
    // Best-effort: tell the shell to wrap up.
    session->terminate(SIGHUP);
    co_await self.scope().all();
    self.draw(AnyLayout{});
}

int run(int /*argc*/, char ** /*argv*/)
{
    return nxt::ui::main(
        [](nxt::ui::UIRuntime & runtime) { nxt::ui::run2(runtime, root); });
}

} // namespace nxt::shell_scope
