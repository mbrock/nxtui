// nxttrace — inspect an nxt trace produced by NXT_TRACE=<path>.
//
// Subcommands:
//   list   <file>              every row, one per line
//   tree   <file>              span tree with timings and row counts
//   frame  <file> <frame_seq>  dump back-buffer raster as ANSI to stdout
//   render <file> <seq> <out>  replay tty bytes up to <seq> through a
//                              fresh vterm, write the screen as a PNG
//   sheet  <file> <out> [N]    render N evenly-spaced frames into one
//                              contact-sheet PNG (default: 9)
//
// All output is line-oriented and grep-friendly so larger queries can
// be expressed by piping through standard tools. For richer querying
// the file is plain Arrow IPC streaming format and can be read by
// pyarrow / duckdb / etc.

#include <nxt/glyph-table.hpp>
#include <nxt/raster.hpp>
#include <nxt/ansi.hpp>
#include <nxt/tui_terminal.hpp>
#include <nxt/units.hpp>
#include <nxt/vterm.hpp>
#include <nxtio/arrow.hpp>

#ifdef NXT_HAVE_PNG
#include <nxt/png.hpp>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

[[nodiscard]] std::string truncate(std::string_view text, std::size_t n)
{
    if (text.size() <= n)
        return std::string{text};
    auto out = std::string{text.substr(0, n)};
    out += "…";
    return out;
}

[[nodiscard]] std::string elide_newlines(std::string_view text)
{
    auto out = std::string{};
    out.reserve(text.size());
    for (auto c : text)
        out.push_back(c == '\n' || c == '\r' ? ' ' : c);
    return out;
}

int cmd_list(const std::string & path)
{
    auto rows = nxt::io::arrow::read_trace_ipc(path);
    std::cout << "rows=" << rows.size() << "\n";
    for (const auto & r : rows) {
        std::cout
            << "seq=" << r.seq
            << " t=" << r.elapsed_ms << "ms"
            << " " << r.phase
            << " " << r.event_type
            << " span=" << (r.span_id.empty() ? "-" : r.span_id)
            << " parent=" << (r.parent_span_id.empty() ? "-" : r.parent_span_id)
            << " name=" << (r.span_name.empty() ? "-" : r.span_name);
        if (r.frame_seq >= 0)
            std::cout << " frame=" << r.frame_seq;
        if (!r.payload_kind.empty())
            std::cout << " kind=" << r.payload_kind
                      << " bin=" << r.payload_bin.size() << "B";
        if (!r.data.empty())
            std::cout << " data=" << truncate(elide_newlines(r.data), 80);
        std::cout << "\n";
    }
    return 0;
}

struct SpanRollup
{
    std::string id;
    std::string parent;
    std::string name;
    std::int64_t begin_ms = -1;
    std::int64_t end_ms = -1;
    std::int64_t rows = 0;
    std::string status;
};

int cmd_tree(const std::string & path)
{
    auto rows = nxt::io::arrow::read_trace_ipc(path);

    std::unordered_map<std::string, SpanRollup> spans;
    std::unordered_map<std::string, std::vector<std::string>> children;

    auto touch = [&](const std::string & id,
                     const std::string & parent,
                     const std::string & name) -> SpanRollup & {
        auto [it, inserted] = spans.try_emplace(id, SpanRollup{id, parent, name});
        if (inserted && !parent.empty())
            children[parent].push_back(id);
        return it->second;
    };

    // First pass: build span_id → rollup. Every row counts towards
    // its `span_id`; span_begin and span_end fill in timing and parent.
    for (const auto & r : rows) {
        if (!r.span_id.empty()) {
            auto & rollup = touch(r.span_id, r.parent_span_id, r.span_name);
            rollup.rows += 1;
            if (r.phase == "span_begin")
                rollup.begin_ms = r.elapsed_ms;
            else if (r.phase == "span_end") {
                rollup.end_ms = r.elapsed_ms;
                rollup.status = r.data;
            }
        }
    }

    // Roots are spans whose parent is empty or whose parent never
    // appeared in the stream. The second case lets us still render a
    // tree from a truncated trace.
    auto roots = std::vector<std::string>{};
    for (const auto & [id, rollup] : spans) {
        if (rollup.parent.empty() || !spans.contains(rollup.parent))
            roots.push_back(id);
    }
    std::sort(roots.begin(), roots.end(), [&](const auto & a, const auto & b) {
        return spans[a].begin_ms < spans[b].begin_ms;
    });

    auto render = [&](auto & self, const std::string & id, std::string prefix,
                      bool last) -> void {
        const auto & s = spans[id];
        std::cout << prefix << (last ? "└─ " : "├─ ")
                  << s.id
                  << "  " << (s.name.empty() ? "(unnamed)" : s.name);
        if (s.begin_ms >= 0 && s.end_ms >= 0) {
            std::cout << "  [" << s.begin_ms << "ms..."
                      << s.end_ms << "ms = "
                      << (s.end_ms - s.begin_ms) << "ms]";
        } else if (s.begin_ms >= 0) {
            std::cout << "  [open from " << s.begin_ms << "ms]";
        }
        std::cout << "  rows=" << s.rows;
        if (!s.status.empty())
            std::cout << "  status=" << s.status;
        std::cout << "\n";

        auto kids = children[id];
        std::sort(kids.begin(), kids.end(),
                  [&](const auto & a, const auto & b) {
                      return spans[a].begin_ms < spans[b].begin_ms;
                  });
        for (std::size_t i = 0; i < kids.size(); ++i)
            self(self, kids[i], prefix + (last ? "   " : "│  "),
                 i + 1 == kids.size());
    };

    for (std::size_t i = 0; i < roots.size(); ++i)
        render(render, roots[i], "", i + 1 == roots.size());

    return 0;
}

int cmd_frame(const std::string & path, std::int64_t frame_seq)
{
    auto rows = nxt::io::arrow::read_trace_ipc(path);
    const nxt::io::arrow::trace_row * hit = nullptr;
    for (const auto & r : rows) {
        if (r.phase == "frame" && r.frame_seq == frame_seq) {
            hit = &r;
            break;
        }
    }
    if (hit == nullptr) {
        std::cerr << "no frame with frame_seq=" << frame_seq
                  << " in " << path << "\n";
        return 2;
    }
    if (hit->payload_kind != "ansi") {
        std::cerr << "unsupported payload_kind: " << hit->payload_kind << "\n";
        return 3;
    }

    // The payload is the raw ANSI byte stream the live terminal saw,
    // so replay is just a `write`. Unicode and styling are preserved
    // because the original renderer already resolved every glyph id.
    const auto & bytes = hit->payload_bin;
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    return 0;
}

struct TtyGeometry
{
    int cols = 80;
    int rows = 24;
};

// Pull the latest geometry from `tty init` / `tty resize` rows up to
// (and including) `seq`. Defaults to 80x24 if none was recorded —
// useful when the producer ran without a real terminal surface but
// the user still wants to feed scripted bytes through a synthetic
// vterm.
TtyGeometry geometry_at(
    const std::vector<nxt::io::arrow::trace_row> & rows,
    std::int64_t seq)
{
    auto geom = TtyGeometry{};
    for (const auto & r : rows) {
        if (r.seq > seq)
            break;
        if (r.phase != "tty")
            continue;
        if (r.event_type != "init" && r.event_type != "resize")
            continue;
        try {
            auto j = nlohmann::json::parse(r.payload_json);
            geom.cols = j.value("cols", geom.cols);
            geom.rows = j.value("rows", geom.rows);
        } catch (...) {
            // Best effort; old or hand-edited traces shouldn't crash.
        }
    }
    return geom;
}

// Replay every `tty bytes` row with `seq <= until_seq` through a
// fresh vterm of the captured geometry. The returned Terminal can be
// rendered into a Raster with render_vterm_screen.
nxt::vterm::Terminal replay_into_vterm(
    const std::vector<nxt::io::arrow::trace_row> & rows,
    std::int64_t until_seq)
{
    auto geom = geometry_at(rows, until_seq);
    nxt::vterm::Terminal vt{geom.rows, geom.cols};
    for (const auto & r : rows) {
        if (r.seq > until_seq)
            break;
        if (r.phase == "tty" && r.event_type == "resize") {
            // Honor mid-run resizes so the replay vterm tracks
            // anything the live terminal saw.
            try {
                auto j = nlohmann::json::parse(r.payload_json);
                auto cols = j.value("cols", geom.cols);
                auto rows_ = j.value("rows", geom.rows);
                vt.set_size(rows_, cols);
            } catch (...) {
            }
            continue;
        }
        if (r.phase != "tty" || r.event_type != "bytes")
            continue;
        if (r.payload_bin.empty())
            continue;
        vt.write(std::string_view{
            reinterpret_cast<const char *>(r.payload_bin.data()),
            r.payload_bin.size()});
    }
    return vt;
}

// Dump libvterm's cell grid as plain text at `seq`. Strips cairo/PNG
// out of the diagnostic chain — if the dump matches what a real
// terminal shows for the same captured byte stream, then any visual
// discrepancy in `render`/`sheet` lives in the PNG path; if the dump
// itself disagrees with the real terminal, libvterm interprets the
// bytes differently than the user's terminal emulator.
int cmd_dump(const std::string & path, std::int64_t seq)
{
    auto rows = nxt::io::arrow::read_trace_ipc(path);
    auto geom = geometry_at(rows, seq);
    auto vt = replay_into_vterm(rows, seq);
    auto [term_rows, term_cols] = vt.get_size();
    std::cout << "# " << geom.cols << "x" << geom.rows
              << " at seq=" << seq << "\n";
    for (int y = 0; y < term_rows; ++y) {
        std::cout << std::format("{:>3} | ", y);
        for (int x = 0; x < term_cols;) {
            auto cell = vt.get_cell(y, x);
            if (!cell) {
                ++x;
                continue;
            }
            // Just emit cell.chars (no styling) so the dump is
            // grep/diff-friendly. Wide chars advance by cell.width.
            int w = std::clamp(cell->width, 1, term_cols - x);
            if (cell->chars.empty()) {
                std::cout << ' ';
            } else {
                for (char32_t cp : cell->chars) {
                    if (cp < 0x80) {
                        std::cout << static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        std::cout << static_cast<char>(0xC0 | (cp >> 6));
                        std::cout << static_cast<char>(0x80 | (cp & 0x3f));
                    } else if (cp < 0x10000) {
                        std::cout << static_cast<char>(0xE0 | (cp >> 12));
                        std::cout << static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                        std::cout << static_cast<char>(0x80 | (cp & 0x3f));
                    } else {
                        std::cout << static_cast<char>(0xF0 | (cp >> 18));
                        std::cout << static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
                        std::cout << static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                        std::cout << static_cast<char>(0x80 | (cp & 0x3f));
                    }
                }
            }
            x += w;
        }
        std::cout << "|\n";
    }
    return 0;
}

#ifdef NXT_HAVE_PNG

int cmd_render(
    const std::string & path,
    std::int64_t seq,
    const std::string & out_path)
{
    auto rows = nxt::io::arrow::read_trace_ipc(path);
    auto geom = geometry_at(rows, seq);
    auto vt = replay_into_vterm(rows, seq);

    nxt::GlyphTable glyphs;
    nxt::Size size{geom.cols * nxt::ch, geom.rows * nxt::ln};
    nxt::Raster raster(size, glyphs);
    auto view = raster.view();
    nxt::tui::render_vterm_screen(view, size, vt, {});
    nxt::png::write(raster, out_path);
    std::cout << "wrote " << out_path
              << "  (seq=" << seq
              << "  " << geom.cols << "x" << geom.rows << ")\n";
    return 0;
}

int cmd_sheet(
    const std::string & path,
    const std::string & out_path,
    int frame_count)
{
    auto rows = nxt::io::arrow::read_trace_ipc(path);
    if (rows.empty()) {
        std::cerr << "trace is empty\n";
        return 2;
    }

    // Pick evenly-spaced seqs across the run. Anchor to `tty bytes`
    // boundaries so each cell shows a coherent state — sampling at
    // seqs that happen to fall on llm/tool rows would still work
    // (replay re-walks every prior byte) but the cells would look
    // identical to nearby `bytes` boundaries.
    std::vector<std::int64_t> byte_seqs;
    for (const auto & r : rows)
        if (r.phase == "tty" && r.event_type == "bytes")
            byte_seqs.push_back(r.seq);
    if (byte_seqs.empty()) {
        std::cerr << "no `tty bytes` rows in trace\n";
        return 3;
    }
    std::vector<std::int64_t> picks;
    picks.reserve(frame_count);
    for (int i = 0; i < frame_count; ++i) {
        auto idx = static_cast<std::size_t>(
            (static_cast<double>(i) / std::max(1, frame_count - 1))
            * (byte_seqs.size() - 1));
        picks.push_back(byte_seqs[idx]);
    }

    // Final geometry: we use the geometry recorded at the LAST seq
    // (in case the run resized). Each pane is `cols x rows` cells.
    auto final_geom = geometry_at(rows, picks.back());
    auto pane_cols = final_geom.cols;
    auto pane_rows = final_geom.rows;

    auto cols_per_row =
        static_cast<int>(std::ceil(std::sqrt(picks.size())));
    auto sheet_rows = (static_cast<int>(picks.size()) + cols_per_row - 1)
        / cols_per_row;

    // Leave a 1-cell gutter between panes so adjacent screens are
    // easy to tell apart at thumbnail resolution.
    auto gutter = 1;
    auto sheet_cols_cells = cols_per_row * pane_cols
        + std::max(0, cols_per_row - 1) * gutter;
    auto sheet_rows_cells = sheet_rows * pane_rows
        + std::max(0, sheet_rows - 1) * gutter;

    nxt::GlyphTable sheet_glyphs;
    nxt::Size sheet_size{
        sheet_cols_cells * nxt::ch, sheet_rows_cells * nxt::ln};
    nxt::Raster sheet(sheet_size, sheet_glyphs);

    // Tint every cell with a soft "gutter" color first. Pane cells
    // will overwrite this when copied in, so the tint only remains
    // visible in the 1-cell gutters between panes (and on any margin
    // around the grid). Picking a desaturated mid-gray rather than
    // pure black makes the edges of each terminal pane pop without
    // distracting from the rendered content.
    constexpr auto gutter_bg = nxt::Rgba8{52, 56, 64};
    {
        auto view = sheet.view();
        for (std::size_t y = 0; y < sheet_rows_cells; ++y) {
            for (std::size_t x = 0; x < sheet_cols_cells; ++x) {
                view.set_bg(
                    nxt::Pos::at(x * nxt::ch, y * nxt::ln),
                    gutter_bg);
            }
        }
    }

    for (std::size_t i = 0; i < picks.size(); ++i) {
        auto geom = geometry_at(rows, picks[i]);
        auto vt = replay_into_vterm(rows, picks[i]);

        // Render this frame into its own raster, then copy cells into
        // the sheet at the right offset. A shared sheet glyph table
        // would skip the copy but each per-frame raster needs its own
        // glyph table anyway because their lifetimes differ.
        nxt::GlyphTable pane_glyphs;
        nxt::Size pane_size{geom.cols * nxt::ch, geom.rows * nxt::ln};
        nxt::Raster pane(pane_size, pane_glyphs);
        auto pane_view = pane.view();
        nxt::tui::render_vterm_screen(pane_view, pane_size, vt, {});

        auto cell_row = static_cast<int>(i) / cols_per_row;
        auto cell_col = static_cast<int>(i) % cols_per_row;
        auto base_x = cell_col * (pane_cols + gutter);
        auto base_y = cell_row * (pane_rows + gutter);

        auto sheet_view = sheet.view();
        for (int y = 0; y < pane_rows; ++y) {
            for (int x = 0; x < pane_cols; ++x) {
                auto src = nxt::Pos::at(x * nxt::ch, y * nxt::ln);
                auto dst = nxt::Pos::at(
                    (base_x + x) * nxt::ch,
                    (base_y + y) * nxt::ln);
                auto cell = pane_view.get_cell(src);
                if (!cell)
                    continue;
                // Re-intern the glyph in the sheet's table since
                // ids are table-local.
                auto bytes = pane_glyphs[cell->glyph];
                auto gid = sheet_glyphs.intern(bytes);
                sheet_view.set_glyph(dst, gid);
                sheet_view.set_fg(dst, cell->fg);
                sheet_view.set_bg(dst, cell->bg);
                sheet_view.set_em(dst, cell->em);
            }
        }
    }

    nxt::png::write(sheet, out_path);
    std::cout << "wrote " << out_path
              << "  (" << picks.size() << " frames, "
              << cols_per_row << "x" << sheet_rows
              << " grid of " << pane_cols << "x" << pane_rows
              << ")\n";
    std::cout << "  seqs:";
    for (auto s : picks)
        std::cout << " " << s;
    std::cout << "\n";
    return 0;
}

#endif // NXT_HAVE_PNG

void usage(const char * prog)
{
    std::cerr
        << "usage:\n"
        << "  " << prog << " list   <trace.arrow>\n"
        << "  " << prog << " tree   <trace.arrow>\n"
        << "  " << prog << " frame  <trace.arrow> <frame_seq>\n"
        << "  " << prog << " dump   <trace.arrow> <seq>\n"
#ifdef NXT_HAVE_PNG
        << "  " << prog << " render <trace.arrow> <seq> <out.png>\n"
        << "  " << prog << " sheet  <trace.arrow> <out.png> [n]\n"
#endif
        ;
}

} // namespace

int main(int argc, char ** argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    auto cmd = std::string_view{argv[1]};
    auto path = std::string{argv[2]};
    try {
        if (cmd == "list")
            return cmd_list(path);
        if (cmd == "tree")
            return cmd_tree(path);
        if (cmd == "frame") {
            if (argc < 4) {
                usage(argv[0]);
                return 1;
            }
            return cmd_frame(path, std::stoll(argv[3]));
        }
        if (cmd == "dump") {
            if (argc < 4) {
                usage(argv[0]);
                return 1;
            }
            return cmd_dump(path, std::stoll(argv[3]));
        }
#ifdef NXT_HAVE_PNG
        if (cmd == "render") {
            if (argc < 5) {
                usage(argv[0]);
                return 1;
            }
            return cmd_render(path, std::stoll(argv[3]), argv[4]);
        }
        if (cmd == "sheet") {
            if (argc < 4) {
                usage(argv[0]);
                return 1;
            }
            int n = argc >= 5 ? std::stoi(argv[4]) : 9;
            return cmd_sheet(path, argv[3], n);
        }
#endif
    } catch (const std::exception & e) {
        std::cerr << "error: " << e.what() << "\n";
        return 10;
    }
    usage(argv[0]);
    return 1;
}
