// cassette.cpp — render an agent trace as a stack of cassette cards.
//
// The XSLT version (cassette/baltic-linen-glow-night.xsl) does this
// transformation through XML templates. This demo is the same thing
// expressed as plain C++ data + composed layout primitives:
//
//   Turn { thought, [Call ...] }
//     Call { name, status, elapsed_ms, args, result, affordances }
//       Result is one of: Matches | Document | Process | Fact | Error
//
//   render_turn  : Turn -> AnyLayout
//   render_call  : Call -> AnyLayout            (header + window +
//   affordances) render_result: Result -> std::optional<AnyLayout>
//
// Each cassette piece is a small function returning an AnyLayout. The
// pieces compose with row/column combinators from nxt::tui; the
// vector-overloads of row/column live in nxt/any_layout.hpp.
//
// The trace itself is one synthetic turn assembled in `sample_turn()`,
// matching cassette/trace-sample.xml line-for-line so the output can be
// diffed against `./cassette/trace-render --intermediate`.

#include <nxt/any_layout.hpp>
#include <nxt/ansi.hpp>
#include <nxt/glyph-table.hpp>
#include <nxt/raster.hpp>
#include <nxt/tui.hpp>
#include <nxt/units.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

namespace nxt::cassette {

using namespace nxt::tui;

// ============================================================================
// Palette — Tailwind's Slate / Amber / Emerald / Orange / Violet / Lime /
// Sky / Rose families at the shades used by baltic-linen-glow-night.
// ============================================================================

constexpr Rgba8 slate_950{2, 6, 23};
constexpr Rgba8 slate_900{15, 23, 42};
constexpr Rgba8 slate_800{30, 41, 59};
constexpr Rgba8 slate_700{51, 65, 85};
constexpr Rgba8 slate_500{100, 116, 139};
constexpr Rgba8 slate_400{148, 163, 184};
constexpr Rgba8 slate_300{203, 213, 225};
constexpr Rgba8 amber_50{255, 251, 235};
constexpr Rgba8 amber_200{253, 230, 138};
constexpr Rgba8 amber_300{252, 211, 77};
constexpr Rgba8 emerald_300{110, 231, 183};
constexpr Rgba8 orange_300{253, 186, 116};
constexpr Rgba8 violet_300{196, 181, 253};
constexpr Rgba8 lime_300{190, 242, 100};
constexpr Rgba8 teal_300{94, 234, 212};
constexpr Rgba8 sky_300{125, 211, 252};
constexpr Rgba8 rose_300{253, 164, 175};

constexpr Rgba8 page_bg = slate_950; // outermost background
constexpr Rgba8 band_bg = slate_900; // header / rack band

// ============================================================================
// Data model — the canonical shape of an agent turn. The fields here
// are what trace-sample.xml carries; the renderer below interprets them.
// ============================================================================

enum class Status { ok, error, pending_approval, running, denied };

struct Field
{
    std::string name;
    std::string value;
};

struct Matches
{
    int total_lines = 0;
    int bytes = 0;
    std::vector<std::string> lines;
};

struct Document
{
    int lines = 0;
    int bytes = 0;
    std::vector<std::string> head;
};

struct StreamLine
{
    bool stderr_ = false;
    std::string text;
};

struct Process
{
    int exit = 0;
    int bytes = 0;
    std::vector<StreamLine> stream;
};

struct Fact
{
    std::vector<Field> fields;
};

struct ErrorResult
{
    std::string message;
};

struct NoResult
{};

using Result =
    std::variant<NoResult, Matches, Document, Process, Fact, ErrorResult>;

struct Affordance
{
    char key;
    std::string label;
};

struct Call
{
    std::string name;
    Status status = Status::ok;
    // Negative means unknown (e.g. pending_approval). The XSLT used the
    // attribute's presence as the toggle.
    int elapsed_ms = -1;
    std::vector<Field> args;
    Result result;
    std::vector<Affordance> affordances;
};

struct Turn
{
    std::string thought;
    std::vector<Call> calls;
};

// ============================================================================
// Tool kinds: display name and accent color for each known tool. The
// XSLT had one xsl:template per kind; here it's a single switch.
// ============================================================================

struct ToolKind
{
    std::string_view display;
    Rgba8 accent;
};

inline ToolKind classify(std::string_view name)
{
    using namespace std::literals;
    if (name == "rg_search"sv)
        return {"find", amber_300};
    if (name == "read_file"sv)
        return {"file", emerald_300};
    if (name == "bash"sv)
        return {"bash", orange_300};
    if (name == "web_fetch"sv)
        return {"fetch", violet_300};
    if (name == "nxt_current_time"sv)
        return {"time", lime_300};
    if (name == "nxt_terminal_size"sv)
        return {"size", lime_300};
    if (name == "nxt_echo"sv)
        return {"echo", lime_300};
    if (name.starts_with("nxt_"sv))
        return {name.substr(4), lime_300};
    return {name, teal_300};
}

// Pick the one argument field that goes on the header line. The XSLT's
// `mode="args-rack"` templates collapsed to the same dispatch.
inline std::string primary_arg(const Call & c)
{
    auto find = [&](std::string_view k) -> std::string {
        for (const auto & f : c.args)
            if (f.name == k)
                return f.value;
        return {};
    };
    if (c.name == "rg_search")
        return find("pattern");
    if (c.name == "read_file")
        return find("path");
    if (c.name == "web_fetch")
        return find("url");
    if (c.name == "nxt_echo")
        return find("text");
    if (c.name == "bash") {
        auto cmd = find("command");
        auto nl = cmd.find('\n');
        if (nl != std::string::npos)
            return cmd.substr(0, nl) + " …";
        return cmd;
    }
    std::string out;
    for (const auto & f : c.args) {
        if (!out.empty())
            out += "  ";
        out += f.name;
        out += '=';
        out += f.value;
    }
    return out;
}

// ============================================================================
// All layout primitives now come from nxt::tui — text/flex_text for
// strings, hfill/flex_fill for bg strips, column/row (and their
// vector-of-AnyLayout overloads in any_layout.hpp) for stacks.
// ============================================================================

// Shorthand: a fixed-width styled chip. Equivalent to
// `text(s, fg(...) | bg(...) | em(...))` but reads better at the dense
// call sites in `spine` and `affordances_strip`.
inline auto chip(
    std::string s,
    Rgba8 fg_color,
    Rgba8 bg_color,
    Emphasis em_flags = DEFAULT_EMPHASIS)
{
    auto style = fg(fg_color) | bg(bg_color);
    if (em_flags != DEFAULT_EMPHASIS)
        style = style | em(em_flags);
    return text(std::move(s), style);
}

// ============================================================================
// Cassette pieces. Each function corresponds to one xsl:template in the
// XSLT version.
// ============================================================================

// Status spine: 3-cell colored block at the leading edge of the header.
// For 'ok', it carries the tool accent (so the band reads as the kind);
// other statuses get a status-meaning chip.
inline auto spine(const Call & c)
{
    auto k = classify(c.name);
    switch (c.status) {
    case Status::ok:
        return chip(" ✓ ", slate_950, k.accent, Emphasis::bold);
    case Status::error:
        return chip(" ! ", slate_950, rose_300, Emphasis::bold);
    case Status::pending_approval:
        return chip(" ? ", slate_950, amber_200, Emphasis::bold);
    case Status::running:
        return chip(" ⠋ ", amber_200, band_bg);
    case Status::denied:
        return chip(" N ", slate_300, slate_500, Emphasis::bold);
    }
    return chip("   ", slate_300, band_bg);
}

// Right-side meta chip in the header row, one of:
//   matches  →  ⨉<total> <kb>K
//   document →  <lines>L <kb>K
//   process  →  exit <code>
inline std::string result_meta(const Result & r)
{
    return std::visit(
        [](const auto & v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Matches>)
                return std::format(
                    "⨉{} {}K", v.total_lines, (v.bytes + 512) / 1024);
            else if constexpr (std::is_same_v<T, Document>)
                return std::format(
                    "{}L {}K", v.lines, (v.bytes + 512) / 1024);
            else if constexpr (std::is_same_v<T, Process>)
                return std::format("exit {}", v.exit);
            else
                return {};
        },
        r);
}

// Header row: status spine, tool name badge, primary arg (stretch),
// elapsed-ms chip, kind-specific meta chip. All on band_bg so the band
// reads as one continuous strip.
inline auto call_header(const Call & c)
{
    auto k = classify(c.name);
    auto meta = result_meta(c.result);
    auto args_text = primary_arg(c);

    std::vector<AnyLayout> pieces;
    pieces.push_back(spine(c));
    pieces.push_back(chip(
        std::format(" {} ", k.display), k.accent, band_bg, Emphasis::bold));
    pieces.push_back(flex_text(args_text, fg(slate_500) | bg(band_bg)));
    if (c.elapsed_ms >= 0) {
        pieces.push_back(
            chip(std::format(" {}ms ", c.elapsed_ms), slate_500, band_bg));
    }
    if (!meta.empty()) {
        pieces.push_back(
            chip(std::format(" {} ", meta), slate_400, band_bg));
    }

    return row(std::move(pieces));
}

// Helper: indent each row by `pad` cells of page background and pad
// the trailing edge with page_bg so the row reads as a continuous band.
// The XSLT achieved this with `px-2` on the .flex-col container.
inline auto window_rows(std::vector<AnyLayout> rows, width_t pad = 2 * ch)
{
    std::vector<AnyLayout> padded;
    padded.reserve(rows.size());
    for (auto & r : rows) {
        padded.push_back(
            row(std::vector<AnyLayout>{
                hfill(pad, page_bg),
                std::move(r),
                flex_fill(page_bg),
            }));
    }
    return column(std::move(padded));
}

// One body line: text on the page background. The outer `surface()`
// already paints page_bg, so `text(s, fg(c))` inherits it correctly.
inline auto body_line(std::string s, Rgba8 fg_color)
{
    return text(std::move(s), fg(fg_color));
}

// match-list and document head share the same template: line + an
// optional "...N more lines." footer in slate_700.
inline auto
linewise_window(const std::vector<std::string> & lines, int total)
{
    std::vector<AnyLayout> rows;
    rows.reserve(lines.size() + 1);
    for (const auto & ln : lines)
        rows.push_back(body_line(ln, slate_300));
    auto shown = static_cast<int>(lines.size());
    if (total > shown) {
        rows.push_back(body_line(
            std::format("...{} more lines.", total - shown), slate_700));
    }
    return window_rows(std::move(rows));
}

inline auto process_window(const Process & p)
{
    std::vector<AnyLayout> rows;
    rows.reserve(p.stream.size());
    for (const auto & s : p.stream) {
        rows.push_back(body_line(s.text, s.stderr_ ? rose_300 : slate_300));
    }
    return window_rows(std::move(rows));
}

inline AnyLayout fact_window(const Fact & f)
{
    // Inline `name: value   name: value` row of fields.
    std::vector<AnyLayout> parts;
    parts.push_back(hfill(2 * ch, page_bg));
    for (std::size_t i = 0; i < f.fields.size(); ++i) {
        const auto & fld = f.fields[i];
        parts.push_back(body_line(fld.name + ":", slate_500));
        parts.push_back(body_line(" ", slate_300));
        parts.push_back(body_line(fld.value, slate_300));
        if (i + 1 < f.fields.size())
            parts.push_back(body_line("  ", slate_300));
    }
    parts.push_back(flex_fill(page_bg));
    return row(std::move(parts));
}

inline AnyLayout error_window(const ErrorResult & e)
{
    return row(
        std::vector<AnyLayout>{
            hfill(2 * ch, page_bg),
            flex_text(e.message, fg(rose_300)),
        });
}

inline std::optional<AnyLayout> render_result(const Result & r)
{
    return std::visit(
        [](const auto & v) -> std::optional<AnyLayout> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, NoResult>)
                return std::nullopt;
            else if constexpr (std::is_same_v<T, Matches>)
                return linewise_window(v.lines, v.total_lines);
            else if constexpr (std::is_same_v<T, Document>)
                return linewise_window(v.head, v.lines);
            else if constexpr (std::is_same_v<T, Process>)
                return process_window(v);
            else if constexpr (std::is_same_v<T, Fact>)
                return fact_window(v);
            else if constexpr (std::is_same_v<T, ErrorResult>)
                return error_window(v);
            else
                return std::nullopt;
        },
        r);
}

// Affordances strip: bracketed keys on band_bg, labels on page_bg, one
// continuous line padded by 2 cells.
inline AnyLayout affordances_strip(const std::vector<Affordance> & a)
{
    std::vector<AnyLayout> parts;
    parts.push_back(hfill(2 * ch, page_bg));
    for (std::size_t i = 0; i < a.size(); ++i) {
        parts.push_back(chip(
            std::string(1, a[i].key),
            amber_200,
            slate_800,
            Emphasis::bold));
        parts.push_back(text(" " + a[i].label, fg(slate_400)));
        if (i + 1 < a.size())
            parts.push_back(text("  "));
    }
    parts.push_back(flex_fill(page_bg));
    return row(std::move(parts));
}

// Thought block: glow-blue prose with a hair of leading padding.
inline AnyLayout thought_block(std::string s)
{
    return row(
        std::vector<AnyLayout>{
            hfill(1 * ch, page_bg),
            flex_text(std::move(s), fg(sky_300)),
        });
}

// A full call cassette: header band, optional result window, optional
// affordances strip — stacked.
inline AnyLayout render_call(const Call & c)
{
    std::vector<AnyLayout> pieces;
    pieces.push_back(call_header(c));
    if (auto window = render_result(c.result))
        pieces.push_back(std::move(*window));
    if (!c.affordances.empty())
        pieces.push_back(affordances_strip(c.affordances));
    return column(std::move(pieces));
}

// The turn: thought (if any) followed by the calls in order.
inline AnyLayout render_turn(const Turn & t)
{
    std::vector<AnyLayout> children;
    if (!t.thought.empty())
        children.push_back(thought_block(t.thought));
    for (const auto & c : t.calls)
        children.push_back(render_call(c));
    return column(std::move(children));
}

// ============================================================================
// Sample data — the same turn as cassette/trace-sample.xml. Each Call
// here mirrors a <call> element there.
// ============================================================================

inline Turn sample_turn()
{
    Turn t;
    t.thought =
        "I need to find where Arrow IPC files are read and figure out "
        "how the schema is structured before deciding on the Parquet "
        "migration path.";

    {
        Call c;
        c.name = "rg_search";
        c.status = Status::ok;
        c.elapsed_ms = 83;
        c.args = {
            {"path", "."},
            {"pattern", "(?i)(parquet|arrow|duckdb)"},
        };
        Matches m;
        m.total_lines = 309;
        m.bytes = 22528;
        m.lines = {
            "./meson.build:39:nanoarrow_ipc_dep = dependency(",
            "./meson.build:40:  'nanoarrow-ipc',",
            "./meson.build:41:  fallback : ['nanoarrow', 'nanoarrow_ipc_dep'],",
            "./meson.build:51:arrow_dataset_deps = [",
        };
        c.result = std::move(m);
        c.affordances = {
            {'A', "see all 309"},
            {'P', "by path"},
            {'C', "context for #1"},
        };
        t.calls.push_back(std::move(c));
    }

    {
        Call c;
        c.name = "read_file";
        c.status = Status::ok;
        c.elapsed_ms = 81;
        c.args = {{"path", "src/nxtio/arrow.hpp"}};
        Document d;
        d.lines = 109;
        d.bytes = 3072;
        d.head = {
            "#pragma once",
            "#include <chrono>",
            "#include <cstdint>",
            "#include <memory>",
        };
        c.result = std::move(d);
        t.calls.push_back(std::move(c));
    }

    {
        Call c;
        c.name = "bash";
        c.status = Status::ok;
        c.elapsed_ms = 240;
        c.args = {
            {"command",
             "duckdb -c \"SELECT count(*) FROM read_arrow('traces/*.arrow')\""},
        };
        Process p;
        p.exit = 0;
        p.bytes = 48;
        p.stream = {
            {false, "┌──────────────┐"},
            {false, "│ count_star() │"},
            {false, "│    141285    │"},
            {false, "└──────────────┘"},
        };
        c.result = std::move(p);
        t.calls.push_back(std::move(c));
    }

    {
        Call c;
        c.name = "nxt_current_time";
        c.status = Status::ok;
        c.elapsed_ms = 2;
        c.args = {};
        Fact f;
        f.fields = {{"local_time", "2026-05-12 14:32:07 +0200"}};
        c.result = std::move(f);
        t.calls.push_back(std::move(c));
    }

    {
        Call c;
        c.name = "bash";
        c.status = Status::error;
        c.elapsed_ms = 35;
        c.args = {
            {"command",
             "cat > /tmp/explore.sql << EOF\nSELECT * FROM ...\nEOF"},
        };
        c.result =
            ErrorResult{"tool execution failed: heredoc inside argument"};
        t.calls.push_back(std::move(c));
    }

    {
        Call c;
        c.name = "web_fetch";
        c.status = Status::pending_approval;
        c.args = {
            {"url", "https://duckdb.org/docs/stable/data/parquet/overview"},
        };
        c.result = NoResult{};
        t.calls.push_back(std::move(c));
    }

    return t;
}

// ============================================================================
// Entry point
// ============================================================================

inline std::size_t terminal_width(std::size_t fallback = 100)
{
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return fallback;
}

int run(int /*argc*/, char ** /*argv*/)
{
    nxt::ansi::mode = nxt::ansi::Mode::enabled;

    auto turn = sample_turn();
    auto inner = render_turn(turn);

    // Drop the inner layout into a Surface so the page bg is painted
    // before any leaf writes its own bg. Leaves that write bg overwrite
    // their cells; cells they leave untouched keep the page color.
    auto layout = surface(
        Style{amber_50, page_bg, DEFAULT_EMPHASIS}, std::move(inner));

    auto width = width_t{terminal_width() * ch};
    auto height = layout.height_hint().min;
    if (height.count() == 0)
        height = 1 * ln;

    GlyphTable glyphs;
    Raster raster{width, height, glyphs};
    auto view = raster.view();
    layout.render(view, raster.extent());

    std::fputs(nxt::ansi::render_raster(raster).c_str(), stdout);
    std::fputc('\n', stdout);
    return 0;
}

} // namespace nxt::cassette
