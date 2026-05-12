// cgroup browser demo: live tree view of /sys/fs/cgroup with
// per-cgroup memory + CPU + I/O stats, sparkline history, and stacked
// memory breakdowns. Each cgroup is its own coroutine; the parent
// composes their surfaces via TreeLayout.
//
// Adapted (and stripped down) from nixb's cgroups.cpp, which used
// mp-units. Here we keep three tiny POD value types (bytes_t,
// usec_t, count_t) local to the demo with their own formatters,
// since nxt/units.hpp is just terminal geometry.

#include <nxt/any_layout.hpp>
#include <nxt/raster.hpp>
#include <nxt/slot.hpp>
#include <nxt/tui.hpp>
#include <nxt/units.hpp>
#include <nxtio/app.hpp>
#include <nxtio/async.hpp>
#include <nxtio/input.hpp>
#include <nxtio/process.hpp>

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
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nxt::cgroup_browser {

using namespace nxt::tui;
using namespace nxt::ui;
using namespace std::chrono_literals;

// ============================================================================
// Domain types — small POD wrappers so the rest of the code reads as
// "bytes" / "microseconds" / "count" rather than naked uint64.
// ============================================================================

struct bytes_t
{
    std::uint64_t v = 0;
};
struct usec_t
{
    std::uint64_t v = 0;
};
struct count_t
{
    std::uint64_t v = 0;
};

inline std::string format_bytes(bytes_t b)
{
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;
    constexpr double tib = gib * 1024.0;
    auto d = static_cast<double>(b.v);
    if (d >= tib)
        return std::format("{:.2f}TiB", d / tib);
    if (d >= gib)
        return std::format("{:.2f}GiB", d / gib);
    if (d >= mib)
        return std::format("{:.1f}MiB", d / mib);
    if (d >= kib)
        return std::format("{:.0f}KiB", d / kib);
    return std::format("{}B", b.v);
}

inline std::string format_count(count_t c)
{
    auto d = static_cast<double>(c.v);
    if (d >= 1e9)
        return std::format("{:.1f}B", d / 1e9);
    if (d >= 1e6)
        return std::format("{:.1f}M", d / 1e6);
    if (d >= 1e3)
        return std::format("{:.1f}k", d / 1e3);
    return std::format("{}", c.v);
}

inline std::string format_time(usec_t u)
{
    auto d = static_cast<double>(u.v);
    if (d >= 1e6)
        return std::format("{:.2f}s", d / 1e6);
    if (d >= 1e3)
        return std::format("{:.1f}ms", d / 1e3);
    return std::format("{}µs", u.v);
}

inline std::string format_percent(double pct)
{
    if (pct < 0.1)
        return "  0%";
    if (pct < 10.0)
        return std::format("{:>3.1f}%", pct);
    return std::format("{:>3.0f}%", pct);
}

// ============================================================================
// Stat encyclopedia: a small subset of the memory.stat / cpu.stat /
// io.stat fields that we actually surface. Each carries a human
// label and a unit kind, matching the spirit of nixb's cgroups.cpp.
// ============================================================================

enum class Unit : std::uint8_t
{
    bytes,
    usec,
    count,
};

struct StatField
{
    std::string_view key;     // raw kernel field name
    std::string_view label;   // human-readable label
    Unit unit;
};

constexpr std::array<StatField, 14> memory_stat_fields = {
    StatField{"anon", "non-file data", Unit::bytes},
    StatField{"file", "cached file data", Unit::bytes},
    StatField{"kernel", "kernel data", Unit::bytes},
    StatField{"kernel_stack", "kernel stack", Unit::bytes},
    StatField{"pagetables", "page tables", Unit::bytes},
    StatField{"slab", "kernel slab", Unit::bytes},
    StatField{"slab_reclaimable", "slab (reclaimable)", Unit::bytes},
    StatField{"slab_unreclaimable", "slab (pinned)", Unit::bytes},
    StatField{"sock", "socket buffers", Unit::bytes},
    StatField{"shmem", "shared memory", Unit::bytes},
    StatField{"anon_thp", "anon huge pages", Unit::bytes},
    StatField{"file_thp", "file huge pages", Unit::bytes},
    StatField{"pgfault", "page faults", Unit::count},
    StatField{"pgmajfault", "major page faults", Unit::count},
};

constexpr std::array<StatField, 6> cpu_stat_fields = {
    StatField{"usage_usec", "total CPU time", Unit::usec},
    StatField{"user_usec", "user mode time", Unit::usec},
    StatField{"system_usec", "kernel mode time", Unit::usec},
    StatField{"nr_periods", "scheduling periods", Unit::count},
    StatField{"nr_throttled", "throttled periods", Unit::count},
    StatField{"throttled_usec", "throttled time", Unit::usec},
};

// ============================================================================
// Parsers — cgroup.* files are tiny ASCII key/value (space-separated).
// ============================================================================

inline std::string read_file_string(
    const std::filesystem::path & path)
{
    auto file = std::ifstream{path};
    if (!file.is_open())
        return {};
    std::string out;
    out.reserve(2048);
    std::string line;
    while (std::getline(file, line)) {
        out += line;
        out += '\n';
    }
    return out;
}

inline std::uint64_t read_uint(const std::filesystem::path & path)
{
    auto file = std::ifstream{path};
    if (!file.is_open())
        return 0;
    std::uint64_t v = 0;
    file >> v;
    return v;
}

inline std::unordered_map<std::string, std::uint64_t>
parse_kv_stat(std::string_view contents)
{
    std::unordered_map<std::string, std::uint64_t> out;
    auto begin = contents.data();
    auto end = begin + contents.size();
    while (begin < end) {
        auto line_end = std::find(begin, end, '\n');
        auto sp = std::find(begin, line_end, ' ');
        if (sp == line_end || sp == begin) {
            begin = line_end + 1;
            continue;
        }
        auto key = std::string{begin, sp};
        auto rest = std::string_view{sp + 1, line_end};
        std::uint64_t v = 0;
        for (auto c : rest) {
            if (c < '0' || c > '9')
                break;
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        out.emplace(std::move(key), v);
        begin = line_end + 1;
    }
    return out;
}

// ============================================================================
// CgroupSnapshot: a single point-in-time read of one cgroup. Stored
// in a small ring buffer per cgroup so we can render sparklines and
// derive rates (e.g., CPU usage = delta_usec / delta_wall_us).
// ============================================================================

struct CgroupSnapshot
{
    std::chrono::steady_clock::time_point sampled;
    bytes_t memory_current{};
    bytes_t memory_max{};       // 0 == unlimited
    bytes_t memory_anon{};
    bytes_t memory_file{};
    bytes_t memory_slab{};
    bytes_t memory_kernel{};
    bytes_t memory_sock{};
    bytes_t memory_other{};     // remainder after the categories above
    usec_t cpu_usage{};
    usec_t cpu_user{};
    usec_t cpu_system{};
    count_t pids{};
};

inline std::uint64_t lookup(
    const std::unordered_map<std::string, std::uint64_t> & m,
    std::string_view key)
{
    auto it = m.find(std::string{key});
    return it == m.end() ? 0 : it->second;
}

inline CgroupSnapshot
read_snapshot(const std::filesystem::path & cgroup_dir)
{
    CgroupSnapshot s;
    s.sampled = std::chrono::steady_clock::now();

    s.memory_current = bytes_t{
        read_uint(cgroup_dir / "memory.current")};

    // memory.max is "max" (literal) for unlimited; ignore parse
    // failure (lazy uint reader returns 0).
    s.memory_max = bytes_t{read_uint(cgroup_dir / "memory.max")};
    s.pids = count_t{read_uint(cgroup_dir / "pids.current")};

    if (auto mem = read_file_string(cgroup_dir / "memory.stat");
        !mem.empty()) {
        auto m = parse_kv_stat(mem);
        s.memory_anon = bytes_t{lookup(m, "anon")};
        s.memory_file = bytes_t{lookup(m, "file")};
        s.memory_slab = bytes_t{lookup(m, "slab")};
        s.memory_kernel = bytes_t{lookup(m, "kernel")};
        s.memory_sock = bytes_t{lookup(m, "sock")};
        auto sum = s.memory_anon.v + s.memory_file.v
                   + s.memory_slab.v + s.memory_kernel.v
                   + s.memory_sock.v;
        s.memory_other = bytes_t{
            s.memory_current.v > sum ? s.memory_current.v - sum
                                     : 0};
    }

    if (auto cpu = read_file_string(cgroup_dir / "cpu.stat");
        !cpu.empty()) {
        auto m = parse_kv_stat(cpu);
        s.cpu_usage = usec_t{lookup(m, "usage_usec")};
        s.cpu_user = usec_t{lookup(m, "user_usec")};
        s.cpu_system = usec_t{lookup(m, "system_usec")};
    }
    return s;
}

// ============================================================================
// Cgroup discovery: walk /sys/fs/cgroup and collect every directory.
// Each cgroup gets a display path, depth, and parent index for tree
// rendering. The order is depth-first so siblings stay grouped.
// ============================================================================

struct CgroupInfo
{
    std::filesystem::path path;
    std::string display_path;  // relative to root
    std::string display_name;  // last segment, or "(root)"
    int depth = 0;
    std::ptrdiff_t parent_idx = -1;
    std::vector<std::size_t> children;
    bool is_last_child = false;
};

inline std::vector<CgroupInfo>
discover_cgroups(const std::filesystem::path & root)
{
    std::vector<CgroupInfo> all;
    std::unordered_map<std::string, std::size_t> by_path;

    auto add = [&](std::filesystem::path p, int depth,
                   std::ptrdiff_t parent) {
        CgroupInfo info;
        info.path = p;
        auto rel = std::filesystem::relative(p, root);
        info.display_path = rel.generic_string();
        if (info.display_path.empty() || info.display_path == ".")
            info.display_path = "/";
        info.display_name = rel.filename().string();
        if (info.display_name.empty())
            info.display_name = "(root)";
        info.depth = depth;
        info.parent_idx = parent;
        by_path.emplace(info.display_path, all.size());
        if (parent >= 0)
            all[static_cast<std::size_t>(parent)]
                .children.push_back(all.size());
        all.push_back(std::move(info));
    };

    add(root, 0, -1);

    // Depth-first traversal so children stay adjacent to their parent.
    auto walk = [&](auto & rec, std::size_t idx) -> void {
        std::vector<std::filesystem::path> subs;
        auto ec = std::error_code{};
        for (auto & entry : std::filesystem::directory_iterator(
                 all[idx].path, ec)) {
            if (ec)
                break;
            if (!entry.is_directory())
                continue;
            // cgroup v2 directories have cgroup.controllers; skip
            // anything else.
            if (!std::filesystem::exists(
                    entry.path() / "cgroup.controllers"))
                continue;
            subs.push_back(entry.path());
        }
        std::sort(subs.begin(), subs.end());
        for (auto & p : subs) {
            auto parent_depth = all[idx].depth;
            auto child_idx = all.size();
            add(p, parent_depth + 1,
                static_cast<std::ptrdiff_t>(idx));
            rec(rec, child_idx);
        }
    };
    walk(walk, 0);

    // Mark is_last_child on every node whose parent's children list
    // ends with it.
    for (auto & info : all) {
        if (!info.children.empty())
            all[info.children.back()].is_last_child = true;
    }
    if (!all.empty())
        all[0].is_last_child = true;

    return all;
}

// ============================================================================
// Per-cgroup rolling state (kept inside each cgroup's coroutine and
// also published into a shared view so the renderer can read it).
// ============================================================================

struct CgroupSamples
{
    static constexpr std::size_t capacity = 32;
    std::deque<CgroupSnapshot> snaps;

    void push(CgroupSnapshot s)
    {
        snaps.push_back(std::move(s));
        while (snaps.size() > capacity)
            snaps.pop_front();
    }

    const CgroupSnapshot & current() const
    {
        return snaps.back();
    }

    bool ready() const
    {
        return !snaps.empty();
    }

    // CPU usage rate as a percent of one core: delta_usec /
    // delta_wall_us. Requires at least two samples.
    double cpu_percent() const
    {
        if (snaps.size() < 2)
            return 0.0;
        const auto & a = snaps[snaps.size() - 2];
        const auto & b = snaps.back();
        auto dt_us = std::chrono::duration_cast<
                         std::chrono::microseconds>(
                         b.sampled - a.sampled)
                         .count();
        if (dt_us <= 0)
            return 0.0;
        auto dcpu_us = static_cast<std::int64_t>(b.cpu_usage.v)
                       - static_cast<std::int64_t>(a.cpu_usage.v);
        if (dcpu_us < 0)
            dcpu_us = 0;
        return 100.0 * static_cast<double>(dcpu_us)
               / static_cast<double>(dt_us);
    }
};

// ============================================================================
// Layout pieces
// ============================================================================

template<Layout Body, Layout Bar>
struct Screen
{
    Body body;
    Bar bar;

    WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    HeightHint height_hint() const
    {
        return HeightHint{
            body.height_hint().min + bar.height_hint().min,
            1.0 * nxt::one,
        };
    }

    void render(RasterView & raster, nxt::Size size) const
    {
        if (size.h.count() == 0)
            return;
        auto bar_h = std::min(bar.height_hint().min, size.h);
        auto body_h = size.h - bar_h;
        if (body_h.count() > 0) {
            auto bs = nxt::Size{size.w, body_h};
            auto sub =
                subraster(raster, nxt::Pos::origin(), bs);
            body.render(sub, bs);
        }
        if (bar_h.count() > 0) {
            auto bs = nxt::Size{size.w, bar_h};
            auto pos = nxt::Pos::origin() + body_h;
            auto sub = subraster(raster, pos, bs);
            bar.render(sub, bs);
        }
    }
};

template<Layout Body, Layout Bar>
auto screen(Body && body, Bar && bar)
{
    return Screen<std::decay_t<Body>, std::decay_t<Bar>>{
        std::forward<Body>(body),
        std::forward<Bar>(bar),
    };
}

// Sparkline using Braille-block height glyphs.
inline std::string sparkline(
    std::span<const double> values, std::size_t cells)
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
    out.reserve(cells * 3);
    auto take = std::min(values.size(), cells);
    // Right-align: pad start with spaces if values shorter than cells.
    auto pad = cells - take;
    for (std::size_t i = 0; i < pad; ++i)
        out += " ";
    auto offset = values.size() - take;
    for (std::size_t i = 0; i < take; ++i) {
        auto v = values[offset + i];
        auto frac = (v - lo) / (hi - lo);
        auto idx = static_cast<std::size_t>(
            std::round(std::clamp(frac, 0.0, 1.0) * 8.0));
        out += blocks[idx];
    }
    return out;
}

// Stacked horizontal bar: each segment is a fraction of total,
// rendered with its own color. Uses background-only painting (no
// glyphs) so labels can be overlaid as text on top of the bar.
// Fills `is_bar_out` (when non-null) with one bool per cell so
// overlay code can pick contrasting fg per cell.
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
    std::vector<std::uint8_t> * is_bar_out = nullptr)
{
    auto w = static_cast<int>(width.count());
    if (w <= 0)
        return;
    if (is_bar_out)
        is_bar_out->assign(static_cast<std::size_t>(w), 0);
    // Pre-fill with track.
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
        double seg_start_cells = acc * w;
        double seg_end_cells = (acc + seg.fraction) * w;
        acc += seg.fraction;
        int i0 = static_cast<int>(std::floor(seg_start_cells));
        int i1 = static_cast<int>(std::ceil(seg_end_cells));
        i0 = std::clamp(i0, 0, w);
        i1 = std::clamp(i1, 0, w);
        for (int i = i0; i < i1; ++i) {
            auto pos = top + i * nxt::ch;
            raster.set_bg(pos, seg.color);
            raster.set_fg(pos, seg.color);
            if (is_bar_out)
                (*is_bar_out)[static_cast<std::size_t>(i)] = 1;
        }
    }
}

// ============================================================================
// Status bar
// ============================================================================

inline auto status_bar(
    std::size_t cgroup_count,
    std::size_t selected,
    std::chrono::steady_clock::time_point started)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [cgroup_count, selected, started](
            RasterView & r, nxt::Size sz) {
            (void) sz;
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            auto line = std::format(
                " cgroup_browser   {} cgroups   selected {}   "
                "uptime {}s   j/k or ↑/↓ scroll · q to quit",
                cgroup_count,
                selected + 1,
                elapsed);
            std::ranges::fill(r.glyphs(), 32);
            std::ranges::fill(r.fgs(), Rgba8{220, 220, 230});
            std::ranges::fill(r.bgs(), Rgba8{30, 34, 40});
            std::ranges::fill(r.ems(), DEFAULT_EMPHASIS);
            r.write_text(nxt::Pos::origin(), line);
        });
}

// ============================================================================
// Shared simulation context — owns the per-cgroup sample buffers so
// both the cgroup coroutines and the renderer can read them. Held
// behind shared_ptr so the published layout outlives root's stack.
// ============================================================================

struct Ctx
{
    std::vector<CgroupInfo> cgroups;
    std::vector<CgroupSamples> samples;  // parallel to cgroups
    std::filesystem::path root;
    std::chrono::steady_clock::time_point started;

    explicit Ctx(std::filesystem::path r)
        : cgroups(discover_cgroups(r))
        , samples(cgroups.size())
        , root(std::move(r))
        , started(std::chrono::steady_clock::now())
    {
    }
};

// ============================================================================
// Cgroup row: two lines.
//   Line 1: tree prefix + name + key numbers (memory, cpu%, pids)
//           + sparkline of recent memory
//   Line 2: tree continuation + stacked memory breakdown bar
// ============================================================================

constexpr Rgba8 col_anon{220, 130, 80};      // orange
constexpr Rgba8 col_file{120, 180, 220};     // sky
constexpr Rgba8 col_slab{160, 200, 110};     // green
constexpr Rgba8 col_kernel{180, 140, 220};   // purple
constexpr Rgba8 col_sock{230, 200, 90};      // yellow
constexpr Rgba8 col_other{140, 150, 170};    // gray
constexpr Rgba8 col_track{32, 36, 46};

// One line per cgroup. The row is a stacked memory bar across the
// whole right side; labels (name, memory total, cpu%, pids) are
// overlaid on top as text. Each label cell's fg is chosen to
// contrast with the bar segment color behind it — dark text over
// the saturated segment colors, light text over the dark track.
inline void render_cgroup_row(
    RasterView & raster,
    nxt::Pos top,
    nxt::width_t width,
    const CgroupInfo & info,
    const CgroupSamples & samples,
    std::span<const std::uint8_t> ancestor_open,
    bool selected,
    int prefix_cells)
{
    auto w = static_cast<int>(width.count());
    auto bg = selected ? Rgba8{40, 50, 70}
                       : (info.depth & 1 ? Rgba8{18, 20, 26}
                                          : Rgba8{14, 16, 22});

    auto sz = nxt::Size{width, 1 * nxt::ln};
    auto row = subraster(raster, top, sz);
    std::ranges::fill(row.glyphs(), 32);
    std::ranges::fill(row.bgs(), bg);
    std::ranges::fill(row.fgs(), Rgba8{210, 215, 225});
    std::ranges::fill(row.ems(), DEFAULT_EMPHASIS);

    // Tree prefix on the far left (1-line form: connector at this
    // depth).
    std::string prefix;
    for (std::size_t k = 0; k < ancestor_open.size(); ++k) {
        bool open = ancestor_open[k] != 0;
        if (k + 1 < ancestor_open.size())
            prefix += open ? "│ " : "  ";
        else
            prefix += info.is_last_child ? "└ " : "├ ";
    }
    if (prefix_cells > 0) {
        auto p_size =
            nxt::Size{prefix_cells * nxt::ch, 1 * nxt::ln};
        auto p_sub =
            subraster(row, nxt::Pos::origin(), p_size);
        std::ranges::fill(p_sub.fgs(), Rgba8{110, 120, 135});
        p_sub.write_text(nxt::Pos::origin(), prefix);
    }

    auto bar_origin =
        nxt::Pos::origin() + prefix_cells * nxt::ch;
    auto bar_w =
        std::max(0, w - prefix_cells);
    if (bar_w == 0)
        return;

    auto name = info.display_name;
    if (info.depth == 0)
        name = "/";

    // Paint the stacked memory bar across the full width.
    std::vector<std::uint8_t> is_bar(
        static_cast<std::size_t>(bar_w), 0);
    bool any_samples = samples.ready();
    if (any_samples && samples.current().memory_current.v > 0) {
        const auto & cur = samples.current();
        auto total = static_cast<double>(cur.memory_current.v);
        auto frac = [&](bytes_t b) {
            return static_cast<double>(b.v) / total;
        };
        std::array<Segment, 6> segs = {
            Segment{frac(cur.memory_anon), col_anon},
            Segment{frac(cur.memory_file), col_file},
            Segment{frac(cur.memory_slab), col_slab},
            Segment{frac(cur.memory_kernel), col_kernel},
            Segment{frac(cur.memory_sock), col_sock},
            Segment{frac(cur.memory_other), col_other},
        };
        paint_stacked_bar(
            row,
            bar_origin,
            bar_w * nxt::ch,
            col_track,
            std::span<const Segment>{segs},
            &is_bar);
    } else {
        // No data yet or empty cgroup: just track.
        for (int i = 0; i < bar_w; ++i) {
            auto pos = bar_origin + i * nxt::ch;
            row.set_bg(pos, col_track);
            row.set_fg(pos, col_track);
        }
    }

    auto fg_over_bar = Rgba8{10, 14, 22};      // dark
    auto fg_over_track = Rgba8{225, 230, 240}; // light
    auto pick_fg = [&](int col) {
        if (col < 0 || col >= bar_w)
            return fg_over_track;
        return is_bar[static_cast<std::size_t>(col)]
                   ? fg_over_bar
                   : fg_over_track;
    };

    // Build the right-aligned tail string.
    std::string tail;
    if (any_samples) {
        const auto & cur = samples.current();
        auto mem_str = format_bytes(cur.memory_current);
        auto cpu_pct = samples.cpu_percent();
        auto cpu_str = format_percent(cpu_pct);
        auto pid_str = std::format("{}p", cur.pids.v);
        tail = std::format(
            " {:>10}  {:>4}  {:>5} ",
            mem_str,
            cpu_str,
            pid_str);
    }

    // Overlay name on the left of the bar.
    auto name_pos = bar_origin + 1 * nxt::ch;
    auto name_max =
        std::max(4, bar_w - static_cast<int>(tail.size()) - 2);
    auto fit_name = name.substr(
        0, std::min<std::size_t>(name.size(), name_max));
    row.write_text(name_pos, fit_name);
    for (std::size_t j = 0; j < fit_name.size(); ++j) {
        auto col =
            1 + static_cast<int>(j);
        row.set_fg(
            bar_origin + col * nxt::ch, pick_fg(col));
    }

    // Overlay the right-aligned tail.
    if (!tail.empty()) {
        auto tail_col = bar_w - static_cast<int>(tail.size());
        if (tail_col >= 0) {
            row.write_text(
                bar_origin + tail_col * nxt::ch, tail);
            for (std::size_t j = 0; j < tail.size(); ++j) {
                auto col = tail_col + static_cast<int>(j);
                row.set_fg(
                    bar_origin + col * nxt::ch, pick_fg(col));
            }
        }
    }
}

// ============================================================================
// Tree layout: visits cgroups in DFS order, computes ancestor-open
// bits per row, and renders each as a two-line block.
// ============================================================================

struct CgroupTreeLayout
{
    std::shared_ptr<Ctx> ctx;
    std::size_t selected = 0;

    WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    HeightHint height_hint() const
    {
        return HeightHint{0 * nxt::ln, 1.0 * nxt::one};
    }

    void render(RasterView & raster, nxt::Size size) const
    {
        auto rows = size.h.count();
        if (rows == 0 || ctx->cgroups.empty())
            return;

        // Find deepest cgroup to size the prefix column.
        int max_depth = 0;
        for (const auto & info : ctx->cgroups)
            max_depth = std::max(max_depth, info.depth);
        auto prefix_cells = max_depth * 2;

        // Scroll so the selected row stays visible. One row per
        // cgroup now (was two for the old stacked-bar layout).
        auto cgroups_visible =
            std::max<std::size_t>(1, rows);
        auto n = ctx->cgroups.size();
        auto half = cgroups_visible / 2;
        std::size_t scroll =
            selected > half ? selected - half : 0;
        if (scroll + cgroups_visible > n)
            scroll = n > cgroups_visible
                         ? n - cgroups_visible
                         : 0;

        auto cursor_origin = nxt::Pos::origin();
        auto cursor = cursor_origin;

        // Need `uint8_t` rather than `bool` so we can take a
        // std::span — std::vector<bool> is a proxy specialization.
        std::vector<std::uint8_t> open_at_depth(
            static_cast<std::size_t>(max_depth + 1), 0);

        for (std::size_t i = 0; i < n; ++i) {
            const auto & info = ctx->cgroups[i];
            if (info.depth > 0)
                open_at_depth[info.depth] =
                    info.is_last_child ? 0 : 1;

            if (i < scroll)
                continue;
            auto used =
                (cursor.y - cursor_origin.y).count();
            if (static_cast<std::int64_t>(rows) - used < 1)
                break;

            std::vector<std::uint8_t> bits;
            bits.reserve(info.depth + 1);
            for (int d = 1; d <= info.depth; ++d)
                bits.push_back(open_at_depth[d]);

            render_cgroup_row(
                raster,
                cursor,
                size.w,
                info,
                ctx->samples[i],
                std::span<const std::uint8_t>{bits},
                i == selected,
                prefix_cells);
            cursor = cursor + 1 * nxt::ln;
        }
    }
};

// ============================================================================
// Per-cgroup coroutine: read snapshot every 500ms, push into the
// shared samples ring, signal damage so the renderer redraws.
// ============================================================================

inline nxt::task<> cgroup_loop(
    yard & self,
    std::shared_ptr<Ctx> ctx,
    std::size_t idx)
{
    constexpr auto interval = 500ms;
    while (!self.cancelled()) {
        auto snap = read_snapshot(ctx->cgroups[idx].path);
        ctx->samples[idx].push(std::move(snap));
        self.runtime().signal_damage();
        co_await self.sleep(interval);
    }
}

// ============================================================================
// Root body
// ============================================================================

inline nxt::task<>
root(yard & self, std::shared_ptr<Ctx> ctx)
{
    if (ctx->cgroups.empty()) {
        self.println("(no cgroups found)");
        co_await next_key_press(self, is_quit_key);
        co_return;
    }

    // Spawn one coroutine per cgroup. ~hundreds is fine.
    std::vector<ProcessHandle> handles;
    handles.reserve(ctx->cgroups.size());
    for (std::size_t i = 0; i < ctx->cgroups.size(); ++i) {
        handles.push_back(self.spawn(
            [ctx, i](yard & s) -> nxt::task<> {
                return cgroup_loop(s, ctx, i);
            }));
    }

    std::size_t selected = 0;
    auto redraw = [&] {
        self.draw(screen(
            CgroupTreeLayout{ctx, selected},
            status_bar(
                ctx->cgroups.size(),
                selected,
                ctx->started)));
    };
    redraw();

    while (!self.cancelled()) {
        auto event = co_await self.next_input();
        if (!event)
            break;
        if (event->type == nxt::input::EventType::release)
            continue;
        if (is_quit_key(*event))
            break;

        bool changed = false;
        if (event->key == nxt::input::Key::down
            || is_character(*event, 'j')) {
            if (selected + 1 < ctx->cgroups.size()) {
                ++selected;
                changed = true;
            }
        } else if (
            event->key == nxt::input::Key::up
            || is_character(*event, 'k')) {
            if (selected > 0) {
                --selected;
                changed = true;
            }
        } else if (
            event->key == nxt::input::Key::home
            || is_character(*event, 'g')) {
            selected = 0;
            changed = true;
        } else if (
            event->key == nxt::input::Key::end
            || is_character(*event, 'G')) {
            selected = ctx->cgroups.size() - 1;
            changed = true;
        }
        if (changed)
            redraw();
    }

    self.cancel();
    co_await self.scope().all();
    self.draw(AnyLayout{});
}

} // namespace nxt::cgroup_browser

int main(int argc, char ** argv)
{
    using namespace nxt::cgroup_browser;

    auto root_path = std::filesystem::path{
        argc > 1 ? argv[1] : "/sys/fs/cgroup"};

    if (!std::filesystem::exists(root_path)) {
        std::fprintf(
            stderr,
            "error: %s does not exist\n",
            root_path.c_str());
        return 1;
    }

    auto ctx = std::make_shared<Ctx>(std::move(root_path));
    std::fprintf(
        stderr,
        "discovered %zu cgroups under %s\n",
        ctx->cgroups.size(),
        ctx->root.c_str());

    return nxt::ui::run2(
        [ctx = std::move(ctx)](yard & self) -> nxt::task<> {
            co_await nxt::cgroup_browser::root(self, ctx);
        });
}
