// Span browser demo: TUI for the parquet-archived OpenTelemetry spans
// in ~/otel-archive/. First iteration: trace list browser. Loads the
// most recent day's spans, groups by trace_id, shows traces sorted by
// start time with a tree+waterfall view of the selection. Reuses the
// layout machinery (TreeLayout-style indenting, BottomAnchor for the
// status bar, OKLCH-blendable colors) from the build_sim demo.

#include <nxt/any_layout.hpp>
#include <nxt/raster.hpp>
#include <nxt/slot.hpp>
#include <nxt/tui.hpp>
#include <nxt/units.hpp>
#include <nxtio/app.hpp>
#include <nxtio/async.hpp>
#include <nxtio/input.hpp>
#include <nxtio/process.hpp>

#include <arrow/api.h>
#include <arrow/compute/initialize.h>
#include <arrow/dataset/api.h>
#include <arrow/dataset/plan.h>
#include <arrow/filesystem/api.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ds = arrow::dataset;
namespace fs = arrow::fs;

namespace nxt::span_browser {

using namespace nxt::tui;
using namespace nxt::ui;
using namespace std::chrono_literals;

// ============================================================================
// Span / Trace data
// ============================================================================

struct Span
{
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id; // empty == root
    std::string name;
    std::string service_name;
    std::int64_t start_unix_nano = 0;
    std::int64_t end_unix_nano = 0;
    std::int64_t duration_us = 0;
    std::string status_code;
    std::string attributes_json;  // raw JSON, parsed on demand
};

struct Trace
{
    std::string trace_id;
    std::vector<std::size_t> span_indices;
    std::int64_t start_unix_nano = 0;
    std::int64_t end_unix_nano = 0;
    std::string root_name;
    // Display-time fields populated by the rename catalog.
    std::string display_label;
    std::string display_annotation;
    std::string service;
    bool has_error = false;
};

struct Dataset
{
    std::vector<Span> spans;
    std::vector<Trace> traces;
};

struct DisplayInfo
{
    std::string label;
    std::string annotation;
};

// Forward declaration — definition lives later with the rest of the
// rename catalog machinery, but `assemble_traces` needs it.
inline DisplayInfo display_info_for(const Span & s);

// ============================================================================
// Parquet loader
// ============================================================================

inline arrow::Result<std::shared_ptr<ds::Dataset>>
open_parquet_dataset(const std::string & path)
{
    auto local_fs = std::make_shared<fs::LocalFileSystem>();
    auto format = std::make_shared<ds::ParquetFileFormat>();
    auto options = ds::FileSystemFactoryOptions{};
    options.partitioning = ds::HivePartitioning::MakeFactory();

    if (std::filesystem::is_regular_file(path)) {
        ARROW_ASSIGN_OR_RAISE(auto info, local_fs->GetFileInfo(path));
        ARROW_ASSIGN_OR_RAISE(
            auto factory,
            ds::FileSystemDatasetFactory::Make(
                local_fs,
                std::vector<fs::FileInfo>{info},
                format,
                options));
        return factory->Finish();
    }

    auto selector = fs::FileSelector{};
    selector.base_dir = path;
    selector.recursive = true;
    ARROW_ASSIGN_OR_RAISE(
        auto factory,
        ds::FileSystemDatasetFactory::Make(
            local_fs, selector, format, options));
    return factory->Finish();
}

inline std::string_view view_at(
    const arrow::StringArray & arr, int64_t i)
{
    if (arr.IsNull(i))
        return {};
    auto v = arr.GetView(i);
    return std::string_view{v.data(), v.size()};
}

inline arrow::Result<std::vector<Span>>
load_spans(const std::string & path)
{
    ARROW_RETURN_NOT_OK(arrow::compute::Initialize());
    ds::internal::Initialize();

    ARROW_ASSIGN_OR_RAISE(auto dataset, open_parquet_dataset(path));

    auto scanner_builder = ds::ScannerBuilder{dataset};
    ARROW_RETURN_NOT_OK(scanner_builder.UseThreads(true));
    ARROW_RETURN_NOT_OK(scanner_builder.Project({
        "trace_id",
        "span_id",
        "parent_span_id",
        "name",
        "service_name",
        "start_unix_nano",
        "end_unix_nano",
        "duration_us",
        "status_code",
        "attributes",
    }));

    ARROW_ASSIGN_OR_RAISE(auto scanner, scanner_builder.Finish());
    ARROW_ASSIGN_OR_RAISE(auto table, scanner->ToTable());

    auto combined = table->CombineChunksToBatch().ValueOrDie();

    auto trace_id_arr =
        std::static_pointer_cast<arrow::StringArray>(
            combined->column(0));
    auto span_id_arr =
        std::static_pointer_cast<arrow::StringArray>(
            combined->column(1));
    auto parent_arr =
        std::static_pointer_cast<arrow::StringArray>(
            combined->column(2));
    auto name_arr =
        std::static_pointer_cast<arrow::StringArray>(
            combined->column(3));
    auto svc_arr =
        std::static_pointer_cast<arrow::StringArray>(
            combined->column(4));
    auto start_arr =
        std::static_pointer_cast<arrow::Int64Array>(
            combined->column(5));
    auto end_arr =
        std::static_pointer_cast<arrow::Int64Array>(
            combined->column(6));
    auto dur_arr =
        std::static_pointer_cast<arrow::Int64Array>(
            combined->column(7));
    auto status_arr =
        std::static_pointer_cast<arrow::StringArray>(
            combined->column(8));
    auto attrs_arr =
        std::static_pointer_cast<arrow::StringArray>(
            combined->column(9));

    auto n = combined->num_rows();
    std::vector<Span> out;
    out.reserve(n);
    for (int64_t i = 0; i < n; ++i) {
        Span s;
        s.trace_id = std::string{view_at(*trace_id_arr, i)};
        s.span_id = std::string{view_at(*span_id_arr, i)};
        s.parent_span_id = std::string{view_at(*parent_arr, i)};
        s.name = std::string{view_at(*name_arr, i)};
        s.service_name = std::string{view_at(*svc_arr, i)};
        s.start_unix_nano = start_arr->IsNull(i)
                                ? 0
                                : start_arr->Value(i);
        s.end_unix_nano =
            end_arr->IsNull(i) ? 0 : end_arr->Value(i);
        s.duration_us =
            dur_arr->IsNull(i) ? 0 : dur_arr->Value(i);
        s.status_code = std::string{view_at(*status_arr, i)};
        s.attributes_json =
            std::string{view_at(*attrs_arr, i)};
        out.push_back(std::move(s));
    }
    return out;
}

inline std::vector<Trace>
assemble_traces(const std::vector<Span> & spans)
{
    std::unordered_map<std::string, std::size_t> index;
    std::vector<Trace> traces;
    traces.reserve(8192);

    for (std::size_t i = 0; i < spans.size(); ++i) {
        const auto & s = spans[i];
        auto it = index.find(s.trace_id);
        std::size_t ti;
        if (it == index.end()) {
            ti = traces.size();
            Trace t;
            t.trace_id = s.trace_id;
            t.start_unix_nano = s.start_unix_nano;
            t.end_unix_nano = s.end_unix_nano;
            t.service = s.service_name;
            traces.push_back(std::move(t));
            index.emplace(s.trace_id, ti);
        } else {
            ti = it->second;
        }
        auto & t = traces[ti];
        t.span_indices.push_back(i);
        if (s.start_unix_nano > 0
            && (t.start_unix_nano == 0
                || s.start_unix_nano < t.start_unix_nano))
            t.start_unix_nano = s.start_unix_nano;
        if (s.end_unix_nano > t.end_unix_nano)
            t.end_unix_nano = s.end_unix_nano;
        if (s.parent_span_id.empty() && !s.name.empty())
            t.root_name = s.name;
        if (s.status_code == "STATUS_CODE_ERROR")
            t.has_error = true;
    }

    // Fill in service / root_name fallbacks for traces with no root,
    // and compute the rename catalog's display fields once per trace.
    for (auto & t : traces) {
        if (t.root_name.empty() && !t.span_indices.empty())
            t.root_name = spans[t.span_indices.front()].name;
        // Find the actual root span (prefer the one with no parent).
        std::ptrdiff_t root_idx = -1;
        for (auto si : t.span_indices) {
            if (spans[si].parent_span_id.empty()) {
                root_idx = static_cast<std::ptrdiff_t>(si);
                break;
            }
        }
        if (root_idx < 0 && !t.span_indices.empty())
            root_idx = static_cast<std::ptrdiff_t>(
                t.span_indices.front());
        if (root_idx >= 0) {
            auto info = display_info_for(
                spans[static_cast<std::size_t>(root_idx)]);
            t.display_label =
                info.label.empty() ? t.root_name : info.label;
            t.display_annotation = std::move(info.annotation);
        } else {
            t.display_label = t.root_name;
        }
    }

    std::sort(
        traces.begin(),
        traces.end(),
        [](const Trace & a, const Trace & b) {
            return a.start_unix_nano > b.start_unix_nano;
        });
    return traces;
}

// ============================================================================
// Formatting helpers
// ============================================================================

inline std::string fit(std::string_view text, std::size_t width)
{
    auto out = std::string{text.substr(0, width)};
    if (text.size() > width && width > 1) {
        out.resize(width - 1);
        out += "…";
    }
    if (out.size() < width)
        out += std::string(width - out.size(), ' ');
    return out;
}

inline std::string short_id(const std::string & id)
{
    return id.substr(0, std::min<std::size_t>(8, id.size()));
}

inline std::string format_duration(std::int64_t us)
{
    if (us < 1000)
        return std::format("{}µs", us);
    if (us < 1'000'000)
        return std::format("{:.1f}ms", us / 1000.0);
    return std::format("{:.2f}s", us / 1'000'000.0);
}

// ============================================================================
// Rename catalog: declarative rules to project OTel data into the
// labels and annotations a human actually wants to read. Each rule
// matches a span by name and produces:
//   * `label`: replaces the span name, with `{attr:k}` placeholders
//     substituted from the span's attributes JSON
//   * `salient`: attribute keys whose values appear after the label
//     as a dim annotation like `[7089 stmts · 1.2MB]`
//
// Adding a rule is the path of least resistance for taming any
// repetitive or jargony span pattern in a real trace.
// ============================================================================

struct DisplayRule
{
    std::string_view match;
    std::string_view label;
    std::vector<std::string_view> salient;
};

inline std::vector<DisplayRule> const & display_rules()
{
    static const std::vector<DisplayRule> rules = {
        {"sheaf.fetch_graph",
         "fetch ▸ {attr:sheaf.graph}",
         {"sheaf.statement_count", "sheaf.statement_bytes"}},
        {"sheaf.repo.load_once",
         "load ▸ {attr:sheaf.graph}",
         {"sheaf.statement_count"}},
        {"sheaf.repo.ask",
         "ask ▸ {attr:sheaf.graph}",
         {"sheaf.row_count"}},
        {"sheaf.repo.match_rows",
         "match ▸ {attr:sheaf.graph}",
         {"sheaf.row_count"}},
        {"quadlog.sqlite.select",
         "sqlite select",
         {"sheaf.row_count"}},
        {"quadlog.sqlite.stream_nquads",
         "sqlite stream nquads",
         {"sheaf.statement_count"}},
        {"Sheaf.Documents.from_rows",
         "documents ◂ rows",
         {"sheaf.row_count"}},
        {"Sheaf.Documents.from_rows.build",
         "documents.build",
         {}},
        {"Sheaf.Documents.list",
         "list documents",
         {"sheaf.row_count"}},
        {"Sheaf.Corpus.find_documents",
         "find documents",
         {"sheaf.row_count"}},
        {"Sheaf.ResourceResolver.resolve",
         "resolve resource",
         {}},
        {"SheafWeb.AssistantMarkdown.document",
         "render markdown",
         {}},
        {"SheafWeb.AssistantMarkdown.resource_ref_resolver",
         "resolve refs in markdown",
         {}},
        {"GET",
         "GET {attr:http.route}",
         {"http.response.status_code"}},
    };
    return rules;
}

// Short labels for salient attribute keys, so an annotation reads
// `7089 stmts` instead of `7089 sheaf.statement_count`.
inline std::string_view short_attr_label(std::string_view key)
{
    if (key == "sheaf.statement_count")
        return "stmts";
    if (key == "sheaf.statement_bytes")
        return "B";
    if (key == "sheaf.row_count")
        return "rows";
    if (key == "sheaf.response_bytes")
        return "B";
    if (key == "http.response.status_code")
        return "";
    if (key == "http.request.method")
        return "";
    if (key == "db.operation")
        return "";
    if (key == "db.system")
        return "";
    // Default: take everything after the last dot.
    auto dot = key.find_last_of('.');
    if (dot != std::string::npos)
        return key.substr(dot + 1);
    return key;
}

inline std::string short_uri(const std::string & uri)
{
    // Strip scheme + host so URIs like https://less.rest/sheaf/foo
    // read as `…/foo` in the small space the label has to work with.
    auto scheme = uri.find("://");
    if (scheme == std::string::npos)
        return uri;
    auto host_start = scheme + 3;
    auto path_start = uri.find('/', host_start);
    if (path_start == std::string::npos)
        return uri.substr(host_start);
    return uri.substr(path_start + 1);
}

inline std::string
expand_template(
    std::string_view tmpl,
    const std::string & span_name,
    const nlohmann::json & attrs)
{
    std::string out;
    out.reserve(tmpl.size());
    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            auto end = tmpl.find('}', i);
            if (end == std::string::npos) {
                out += tmpl[i++];
                continue;
            }
            auto inner = tmpl.substr(i + 1, end - i - 1);
            if (inner == "name") {
                out += span_name;
            } else if (inner.starts_with("attr:")) {
                auto key = std::string{inner.substr(5)};
                if (attrs.is_object() && attrs.contains(key)) {
                    const auto & v = attrs[key];
                    if (v.is_string()) {
                        auto s = v.get<std::string>();
                        // Compact URIs in labels — they otherwise
                        // dominate horizontal space.
                        if (s.find("://") != std::string::npos)
                            out += short_uri(s);
                        else
                            out += s;
                    } else {
                        out += v.dump();
                    }
                } else {
                    // No such attribute — leave a hint marker.
                    out += "—";
                }
            }
            i = end + 1;
        } else {
            out += tmpl[i++];
        }
    }
    return out;
}

inline std::string
format_salient(
    const nlohmann::json & attrs,
    const std::vector<std::string_view> & keys)
{
    if (!attrs.is_object())
        return {};
    std::vector<std::string> parts;
    parts.reserve(keys.size());
    for (auto key : keys) {
        auto k = std::string{key};
        if (!attrs.contains(k))
            continue;
        const auto & v = attrs[k];
        std::string vstr;
        if (v.is_string())
            vstr = v.get<std::string>();
        else if (v.is_number_integer())
            vstr = std::to_string(v.get<std::int64_t>());
        else if (v.is_number_unsigned())
            vstr = std::to_string(v.get<std::uint64_t>());
        else if (v.is_number_float())
            vstr = std::format("{:.2f}", v.get<double>());
        else if (v.is_boolean())
            vstr = v.get<bool>() ? "true" : "false";
        else
            vstr = v.dump();
        auto label = short_attr_label(key);
        if (label.empty())
            parts.push_back(std::move(vstr));
        else
            parts.push_back(
                std::format("{} {}", vstr, label));
    }
    if (parts.empty())
        return {};
    std::string s;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            s += " · ";
        s += parts[i];
    }
    return s;
}

inline nlohmann::json parse_attrs(const Span & s)
{
    if (s.attributes_json.empty()
        || s.attributes_json == "null")
        return nlohmann::json::object();
    try {
        return nlohmann::json::parse(s.attributes_json);
    } catch (...) {
        return nlohmann::json::object();
    }
}

inline DisplayInfo display_info_for(const Span & s)
{
    auto attrs = parse_attrs(s);
    for (const auto & rule : display_rules()) {
        if (s.name == rule.match) {
            return {
                expand_template(rule.label, s.name, attrs),
                format_salient(attrs, rule.salient),
            };
        }
    }
    return {s.name, format_salient(attrs, {})};
}

inline std::string format_local_time(std::int64_t unix_nano)
{
    if (unix_nano == 0)
        return "—";
    auto sys =
        std::chrono::system_clock::time_point{
            std::chrono::nanoseconds{unix_nano}};
    auto floor =
        std::chrono::floor<std::chrono::seconds>(sys);
    return std::format("{:%H:%M:%S}", floor);
}

// ============================================================================
// Tree assembly for a single trace
// ============================================================================

struct TraceTree
{
    // Display order: DFS pre-order through visible-tree (which = all
    // spans here, since the user picked one trace).
    std::vector<std::size_t> order;
    // Per-span (indexed by position in trace.span_indices):
    std::vector<std::ptrdiff_t> parent_pos;   // -1 for roots
    std::vector<int> depth;
    std::vector<bool> is_last_child;
    int max_depth = 0;
};

inline TraceTree
build_trace_tree(const Dataset & data, const Trace & trace)
{
    TraceTree t;
    const auto n = trace.span_indices.size();
    t.parent_pos.assign(n, -1);
    t.depth.assign(n, 0);
    t.is_last_child.assign(n, false);

    // Map span_id -> position within this trace.
    std::unordered_map<std::string_view, std::size_t> by_id;
    by_id.reserve(n);
    for (std::size_t pos = 0; pos < n; ++pos) {
        const auto & s = data.spans[trace.span_indices[pos]];
        by_id.emplace(std::string_view{s.span_id}, pos);
    }

    std::vector<std::vector<std::size_t>> children(n);
    std::vector<bool> has_parent(n, false);
    for (std::size_t pos = 0; pos < n; ++pos) {
        const auto & s = data.spans[trace.span_indices[pos]];
        if (s.parent_span_id.empty())
            continue;
        auto it =
            by_id.find(std::string_view{s.parent_span_id});
        if (it == by_id.end())
            continue;
        t.parent_pos[pos] =
            static_cast<std::ptrdiff_t>(it->second);
        children[it->second].push_back(pos);
        has_parent[pos] = true;
    }

    // Sort each parent's children by start time for waterfall sanity.
    for (auto & c : children) {
        std::sort(
            c.begin(),
            c.end(),
            [&](std::size_t a, std::size_t b) {
                return data.spans[trace.span_indices[a]]
                           .start_unix_nano
                       < data.spans[trace.span_indices[b]]
                             .start_unix_nano;
            });
    }
    // is_last_child: last entry in each children list is last.
    for (auto & c : children) {
        if (!c.empty())
            t.is_last_child[c.back()] = true;
    }

    // Roots: spans without a recognized parent.
    std::vector<std::size_t> roots;
    for (std::size_t pos = 0; pos < n; ++pos)
        if (!has_parent[pos])
            roots.push_back(pos);
    std::sort(
        roots.begin(),
        roots.end(),
        [&](std::size_t a, std::size_t b) {
            return data.spans[trace.span_indices[a]]
                       .start_unix_nano
                   < data.spans[trace.span_indices[b]]
                         .start_unix_nano;
        });
    if (!roots.empty())
        t.is_last_child[roots.back()] = true;

    // DFS preorder.
    t.order.reserve(n);
    auto dfs = [&](auto & rec, std::size_t pos, int d) -> void {
        t.depth[pos] = d;
        if (d > t.max_depth)
            t.max_depth = d;
        t.order.push_back(pos);
        for (auto c : children[pos])
            rec(rec, c, d + 1);
    };
    for (auto r : roots)
        dfs(dfs, r, 0);
    return t;
}

// ============================================================================
// Waterfall painter: paints a 1-row bar into an existing raster
// region, with proper sub-cell rendering at both edges. The left
// edge uses an inverse-color block trick to fill the right portion
// of a partial cell (fg=track over bg=bar), since the standard
// Unicode block elements are all left-aligned.
//
// Returns per-cell info about whether each cell is "in the bar" so
// label-overlay code can pick contrasting text colors.
// ============================================================================

enum class CellKind : std::uint8_t
{
    track,    // entirely outside the bar
    bar,      // entirely inside the bar
    edge_l,   // partial: bar enters mid-cell (bar on the right)
    edge_r,   // partial: bar exits mid-cell (bar on the left)
};

struct BarCell
{
    CellKind kind;
    // Fraction of the cell covered by the bar (for partials).
    float bar_fill = 0.0f;
};

inline std::vector<BarCell> compute_bar_cells(
    double start_frac, double end_frac, int width_cells)
{
    std::vector<BarCell> out(width_cells);
    if (width_cells <= 0)
        return out;
    auto clamp01 = [](double x) {
        return std::clamp(x, 0.0, 1.0);
    };
    start_frac = clamp01(start_frac);
    end_frac = clamp01(end_frac);
    if (end_frac < start_frac)
        std::swap(start_frac, end_frac);

    double w = static_cast<double>(width_cells);
    double s = start_frac * w;
    double e = end_frac * w;
    if (e - s < 0.125 && end_frac > 0.0)
        e = s + 0.125;

    for (int i = 0; i < width_cells; ++i) {
        double cs = static_cast<double>(i);
        double ce = cs + 1.0;
        double left = std::max(s, cs);
        double right = std::min(e, ce);
        double fill = std::max(0.0, right - left);
        if (left <= cs && right >= ce) {
            out[i] = {CellKind::bar, 1.0f};
        } else if (fill <= 0.0) {
            out[i] = {CellKind::track, 0.0f};
        } else if (left > cs && right >= ce) {
            // Bar enters at left > cs but extends to cell_end:
            // partial on the LEFT edge of the bar.
            out[i] = {CellKind::edge_l,
                      static_cast<float>(fill)};
        } else if (left <= cs && right < ce) {
            // Bar exits before cell_end: partial on the RIGHT
            // edge of the bar.
            out[i] = {CellKind::edge_r,
                      static_cast<float>(fill)};
        } else {
            // Island: bar fits entirely inside this cell. Treat as
            // right edge for rendering (fill from left).
            out[i] = {CellKind::edge_r,
                      static_cast<float>(fill)};
        }
    }
    return out;
}

constexpr std::array<std::string_view, 9> bar_blocks = {
    " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█",
};

// Paint one row's bar with proper edge rendering. `track_color` is
// the empty-portion bg; `bar_color` is the fill. Each cell is
// written with the correct glyph + fg/bg so that label overlay later
// can read `cells[i].kind` to pick a contrasting fg.
inline void paint_bar_row(
    RasterView & raster,
    nxt::Pos top,
    nxt::width_t width,
    const std::vector<BarCell> & cells,
    Rgba8 bar_color,
    Rgba8 track_color)
{
    auto w = static_cast<int>(width.count());
    for (int i = 0; i < w && i < static_cast<int>(cells.size());
         ++i) {
        auto pos = top + i * nxt::ch;
        const auto & c = cells[i];
        switch (c.kind) {
        case CellKind::track:
            // Empty: bg = track, glyph = space (already cleared).
            raster.set_bg(pos, track_color);
            raster.set_fg(pos, track_color);
            break;
        case CellKind::bar:
            // Full block of bar color. Use bg so the cell stays a
            // solid color regardless of any glyph we later overlay.
            raster.set_bg(pos, bar_color);
            raster.set_fg(pos, bar_color);
            // Glyph stays as space — bg paints the whole cell.
            break;
        case CellKind::edge_l: {
            // Bar covers the right portion of the cell. Render an
            // inverted left-aligned block: glyph fg=track, bg=bar.
            // The glyph fills `1 - bar_fill` from the left with
            // track color; the rest of the cell shows bar color.
            float track_part = 1.0f - c.bar_fill;
            auto idx = static_cast<std::size_t>(
                std::round(track_part * 8.0f));
            idx = std::clamp<std::size_t>(idx, 1, 8);
            // Write the block char.
            raster.write_text(pos, std::string{bar_blocks[idx]});
            raster.set_fg(pos, track_color);
            raster.set_bg(pos, bar_color);
            break;
        }
        case CellKind::edge_r: {
            // Bar covers the left portion of the cell. Standard
            // left-aligned block: fg=bar, bg=track.
            auto idx = static_cast<std::size_t>(
                std::round(c.bar_fill * 8.0f));
            idx = std::clamp<std::size_t>(idx, 1, 8);
            raster.write_text(pos, std::string{bar_blocks[idx]});
            raster.set_fg(pos, bar_color);
            raster.set_bg(pos, track_color);
            break;
        }
        }
    }
}

// Whether the cell at column `col` reads as bar-colored (for picking
// label fg contrast).
inline bool is_bar_cell(const std::vector<BarCell> & cells, int col)
{
    if (col < 0 || col >= static_cast<int>(cells.size()))
        return false;
    auto k = cells[col].kind;
    return k == CellKind::bar || k == CellKind::edge_l;
}

inline Rgba8 duration_color(std::int64_t us)
{
    if (us < 1000)
        return Rgba8{120, 200, 180};  // teal: <1ms
    if (us < 100'000)
        return Rgba8{230, 200, 90};   // yellow: <100ms
    if (us < 1'000'000)
        return Rgba8{240, 150, 80};   // orange: <1s
    return Rgba8{230, 110, 110};      // red: >=1s
}

// ============================================================================
// Trace list layout: scrollable column of trace rows
// ============================================================================

// Two-line trace block: name on the headline, metadata on the
// second line in a dimmer color. Selection highlights both rows.
inline void render_trace_block(
    RasterView & raster,
    nxt::Pos top,
    nxt::width_t width,
    const Trace & t,
    bool selected,
    bool stripe)
{
    auto bg_base = stripe ? Rgba8{22, 24, 30} : Rgba8{16, 18, 24};
    auto bg = selected ? Rgba8{45, 60, 90} : bg_base;
    auto fg_name = t.has_error ? Rgba8{240, 140, 140}
                               : Rgba8{225, 230, 240};
    auto fg_meta = Rgba8{130, 140, 160};
    auto fg_id = Rgba8{120, 180, 220};
    auto total_us =
        (t.end_unix_nano - t.start_unix_nano) / 1000;
    auto fg_dur = duration_color(total_us);

    auto paint_row =
        [&](nxt::Pos pos, std::string_view line, Rgba8 default_fg)
        -> RasterView {
        auto sz = nxt::Size{width, 1 * nxt::ln};
        auto sub = subraster(raster, pos, sz);
        std::ranges::fill(sub.glyphs(), 32);
        std::ranges::fill(sub.bgs(), bg);
        std::ranges::fill(sub.fgs(), default_fg);
        std::ranges::fill(sub.ems(), DEFAULT_EMPHASIS);
        sub.write_text(nxt::Pos::origin(), std::string{line});
        return sub;
    };

    // Headline: cursor + root name. Name gets bold via the fg
    // bright color but no explicit Emphasis::bold to keep the row
    // height visually calm.
    auto w = static_cast<int>(width.count());
    auto label = t.display_label.empty() ? t.root_name
                                         : t.display_label;
    auto headline_name_w = std::max(8, w - 4);
    auto headline = std::format(
        " {} {}",
        selected ? "▸" : " ",
        fit(label, static_cast<std::size_t>(headline_name_w)));
    auto head_sub = paint_row(top, headline, fg_name);
    if (selected) {
        head_sub.set_fg(
            nxt::Pos::origin() + 1 * nxt::ch,
            Rgba8{255, 210, 90});
    }

    // Meta line: id · 3sp · 1.4ms · 20:10:18 · annotation
    auto sub_top = top + 1 * nxt::ln;
    auto meta = std::format(
        "   {}  {}sp  {}  {}",
        short_id(t.trace_id),
        t.span_indices.size(),
        format_duration(total_us),
        format_local_time(t.start_unix_nano));
    if (!t.display_annotation.empty())
        meta += "  · " + t.display_annotation;
    auto meta_sub = paint_row(sub_top, meta, fg_meta);

    // Re-color id segment (cyan) and duration segment.
    auto o = nxt::Pos::origin();
    for (int j = 0; j < 8; ++j)
        meta_sub.set_fg(o + (3 + j) * nxt::ch, fg_id);

    // The duration field starts after "   12345678  3sp  ":
    //   3 spaces + 8 id + 2 + 3 count + "sp" (count rendered with
    //   `{}sp` so width depends on count digits; assume <= 4 digits
    //   for the highlight band).
    auto count_str = std::to_string(t.span_indices.size());
    auto dur_col = 3 + 8 + 2 + static_cast<int>(count_str.size())
                   + 2 + 2;
    auto dur_str = format_duration(total_us);
    for (std::size_t j = 0;
         j < dur_str.size() && dur_col + static_cast<int>(j) < w;
         ++j) {
        meta_sub.set_fg(
            o + (dur_col + static_cast<int>(j)) * nxt::ch, fg_dur);
    }
}

inline auto status_bar(
    std::size_t selected_idx,
    std::size_t total)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [selected_idx, total](RasterView & r, nxt::Size sz) {
            (void) sz;
            auto line = std::format(
                " span_browser   {}/{} traces   j/k or ↑/↓ scroll, "
                "q to quit",
                selected_idx + 1,
                total);
            std::ranges::fill(r.glyphs(), 32);
            std::ranges::fill(r.fgs(), Rgba8{220, 220, 230});
            std::ranges::fill(r.bgs(), Rgba8{30, 34, 40});
            std::ranges::fill(r.ems(), DEFAULT_EMPHASIS);
            r.write_text(nxt::Pos::origin(), line);
        });
}

// Trace list layout: each trace gets 2 rows (name on top, meta
// below). Scroll measured in traces, not rows. Selected trace
// stays near the middle of the visible band.
struct TraceListLayout
{
    std::span<const Trace> traces;
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
        auto rows = static_cast<std::size_t>(size.h.count());
        if (rows == 0 || traces.empty())
            return;

        auto traces_visible = rows / 2;
        if (traces_visible == 0)
            traces_visible = 1;

        auto n = traces.size();
        auto half = traces_visible / 2;
        std::size_t scroll =
            selected > half ? selected - half : 0;
        if (scroll + traces_visible > n)
            scroll = n > traces_visible ? n - traces_visible : 0;

        auto origin_pos = nxt::Pos::origin();
        auto cursor = origin_pos;
        for (std::size_t k = 0;
             k < traces_visible && scroll + k < n;
             ++k) {
            auto idx = scroll + k;
            // Block needs 2 rows; bail at the bottom edge.
            auto used = (cursor.y - origin_pos.y).count();
            auto rows_left =
                static_cast<std::int64_t>(rows) - used;
            if (rows_left < 2)
                break;
            render_trace_block(
                raster,
                cursor,
                size.w,
                traces[idx],
                idx == selected,
                (idx & 1) == 1);
            cursor = cursor + 2 * nxt::ln;
        }
    }
};

// ============================================================================
// Trace detail pane: tree of spans with time-aligned waterfall
// ============================================================================

// Pick the "focus" span to show attributes for: prefer the root, or
// the longest span if multiple roots, or the first if none.
inline std::size_t
pick_focus_span(const Dataset & data, const Trace & trace)
{
    std::ptrdiff_t best = -1;
    std::int64_t best_dur = -1;
    for (auto idx : trace.span_indices) {
        const auto & s = data.spans[idx];
        if (s.parent_span_id.empty()) {
            // Root: pick the one with the largest duration.
            if (s.duration_us > best_dur) {
                best_dur = s.duration_us;
                best = static_cast<std::ptrdiff_t>(idx);
            }
        }
    }
    if (best >= 0)
        return static_cast<std::size_t>(best);
    // No root: pick longest overall.
    for (auto idx : trace.span_indices) {
        if (data.spans[idx].duration_us > best_dur) {
            best_dur = data.spans[idx].duration_us;
            best = static_cast<std::ptrdiff_t>(idx);
        }
    }
    return best >= 0 ? static_cast<std::size_t>(best)
                     : trace.span_indices.front();
}

struct TraceDetailLayout
{
    std::shared_ptr<const Dataset> data;
    std::size_t trace_idx = 0;

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
        if (trace_idx >= data->traces.size())
            return;
        const auto & trace = data->traces[trace_idx];
        if (trace.span_indices.empty())
            return;

        auto tree = build_trace_tree(*data, trace);

        // Header line: trace id + span count + total duration.
        auto header_h = 1 * nxt::ln;
        if (size.h.count() < 1)
            return;
        auto total_us =
            (trace.end_unix_nano - trace.start_unix_nano) / 1000;
        {
            auto h_sub = subraster(
                raster,
                nxt::Pos::origin(),
                nxt::Size{size.w, header_h});
            std::ranges::fill(h_sub.glyphs(), 32);
            std::ranges::fill(h_sub.bgs(), Rgba8{20, 26, 36});
            std::ranges::fill(h_sub.fgs(), Rgba8{220, 230, 240});
            std::ranges::fill(h_sub.ems(), Emphasis::bold);
            auto line = std::format(
                " trace {}  {} spans  {}",
                short_id(trace.trace_id),
                trace.span_indices.size(),
                format_duration(total_us));
            h_sub.write_text(nxt::Pos::origin(), line);
        }

        // Body: tree + waterfall, one row per span (truncate at
        // available height).
        const auto rows_avail =
            size.h.count() - header_h.count();
        if (rows_avail <= 0)
            return;

        // One line per span. Layout per row:
        //   [prefix tree chars] [waterfall bar with name + duration
        //    overlaid as text]
        // The label characters use a per-cell contrasting fg color
        // chosen based on whether the cell behind them is "bar" or
        // "track".
        auto prefix_cells = tree.max_depth * 2;
        auto inner_w = static_cast<int>(size.w.count());
        auto bar_cells = std::max(8, inner_w - prefix_cells - 1);

        auto trace_dur_ns = static_cast<double>(
            trace.end_unix_nano - trace.start_unix_nano);
        if (trace_dur_ns <= 0.0)
            trace_dur_ns = 1.0;

        auto track_color = Rgba8{36, 40, 50};

        auto body_top = nxt::Pos::origin() + header_h;
        auto cursor = body_top;
        for (std::size_t i = 0; i < tree.order.size(); ++i) {
            auto used = (cursor.y - body_top.y).count();
            auto rows_left =
                static_cast<std::int64_t>(rows_avail) - used;
            if (rows_left < 1)
                break;

            auto pos = tree.order[i];
            const auto & s =
                data->spans[trace.span_indices[pos]];

            // Clear the row.
            auto row_size = nxt::Size{size.w, 1 * nxt::ln};
            auto row = subraster(raster, cursor, row_size);
            std::ranges::fill(row.glyphs(), 32);
            std::ranges::fill(row.bgs(), Rgba8{18, 20, 26});
            std::ranges::fill(row.fgs(), Rgba8{200, 205, 215});
            std::ranges::fill(row.ems(), DEFAULT_EMPHASIS);

            // Build prefix (line 1 form: elbow at this depth).
            std::vector<bool> ancestor_bits;
            {
                auto cur = tree.parent_pos[pos];
                while (cur >= 0) {
                    ancestor_bits.push_back(
                        tree.is_last_child[
                            static_cast<std::size_t>(cur)]);
                    cur = tree.parent_pos[
                        static_cast<std::size_t>(cur)];
                }
                std::reverse(
                    ancestor_bits.begin(),
                    ancestor_bits.end());
            }
            std::string prefix_str;
            for (std::size_t k = 1; k < ancestor_bits.size();
                 ++k)
                prefix_str +=
                    ancestor_bits[k] ? "  " : "│ ";
            if (!ancestor_bits.empty())
                prefix_str +=
                    tree.is_last_child[pos] ? "└ " : "├ ";

            if (prefix_cells > 0) {
                auto p_size = nxt::Size{
                    prefix_cells * nxt::ch, 1 * nxt::ln};
                auto p_sub = subraster(
                    row, nxt::Pos::origin(), p_size);
                std::ranges::fill(
                    p_sub.fgs(), Rgba8{110, 120, 135});
                p_sub.write_text(
                    nxt::Pos::origin(), prefix_str);
            }

            // Compute and paint the bar across the rest of the row.
            auto bar_color = duration_color(s.duration_us);
            auto start_frac =
                (static_cast<double>(s.start_unix_nano)
                 - static_cast<double>(trace.start_unix_nano))
                / trace_dur_ns;
            auto end_frac =
                (static_cast<double>(s.end_unix_nano)
                 - static_cast<double>(trace.start_unix_nano))
                / trace_dur_ns;
            auto cells = compute_bar_cells(
                start_frac, end_frac, bar_cells);
            auto bar_origin =
                nxt::Pos::origin() + prefix_cells * nxt::ch;
            paint_bar_row(
                row,
                bar_origin,
                bar_cells * nxt::ch,
                cells,
                bar_color,
                track_color);

            // Overlay name (left of bar region) and duration
            // (right of bar region) as text. Pick fg per cell so
            // labels stay readable on bar OR on track.
            auto fg_dark = Rgba8{14, 16, 22};
            auto fg_light = Rgba8{225, 230, 240};

            auto pick_fg = [&](int col) {
                return is_bar_cell(cells, col)
                           ? fg_dark
                           : fg_light;
            };

            // Name + annotation: " label · annotation"
            // Renamed label and salient suffix from the catalog.
            auto info = display_info_for(s);
            std::string name_str = " " + info.label;
            if (!info.annotation.empty())
                name_str += "  · " + info.annotation;
            auto max_name_cells =
                std::max(4, bar_cells - 10);
            auto trimmed = fit(
                name_str,
                static_cast<std::size_t>(max_name_cells));
            row.write_text(bar_origin, trimmed);
            for (int j = 0;
                 j < static_cast<int>(trimmed.size()) && j < bar_cells;
                 ++j) {
                // utf8 isn't strictly j == col but for ASCII names
                // it's fine; non-ASCII span names will get slightly
                // miscolored. Acceptable for v0.
                row.set_fg(
                    bar_origin + j * nxt::ch, pick_fg(j));
            }

            // Duration: right-aligned in the bar region.
            auto dur_str = format_duration(s.duration_us);
            auto dur_w = static_cast<int>(dur_str.size());
            auto dur_col = bar_cells - dur_w - 1;
            if (dur_col > 0) {
                row.write_text(
                    bar_origin + dur_col * nxt::ch, dur_str);
                for (int j = 0; j < dur_w; ++j) {
                    row.set_fg(
                        bar_origin + (dur_col + j) * nxt::ch,
                        pick_fg(dur_col + j));
                }
            }

            cursor = cursor + 1 * nxt::ln;
        }
    }
};

// ============================================================================
// Attribute panel: parses the focus span's JSON attributes and
// shows them as a two-column key/value list. Renders below the
// detail tree with a separator. Long values are truncated.
// ============================================================================

struct AttributePanel
{
    std::shared_ptr<const Dataset> data;
    std::size_t trace_idx = 0;

    WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    HeightHint height_hint() const
    {
        // Hint: a sensible default for the focus span's attrs;
        // actual rendering will fill whatever it gets. Use flex so
        // the column above us can shrink if needed.
        return HeightHint{10 * nxt::ln, 1.0 * nxt::one};
    }

    void render(RasterView & raster, nxt::Size size) const
    {
        if (size.h.count() == 0)
            return;
        if (trace_idx >= data->traces.size())
            return;
        const auto & trace = data->traces[trace_idx];
        if (trace.span_indices.empty())
            return;

        std::ranges::fill(raster.glyphs(), 32);
        std::ranges::fill(raster.bgs(), Rgba8{14, 16, 22});
        std::ranges::fill(raster.fgs(), Rgba8{200, 205, 215});
        std::ranges::fill(raster.ems(), DEFAULT_EMPHASIS);

        auto focus = pick_focus_span(*data, trace);
        const auto & s = data->spans[focus];

        // Header line.
        {
            auto h_size = nxt::Size{size.w, 1 * nxt::ln};
            auto h_sub = subraster(
                raster, nxt::Pos::origin(), h_size);
            std::ranges::fill(h_sub.bgs(), Rgba8{22, 28, 40});
            std::ranges::fill(h_sub.fgs(), Rgba8{230, 235, 245});
            auto line = std::format(
                " attributes  {}  {}",
                s.name,
                format_duration(s.duration_us));
            h_sub.write_text(nxt::Pos::origin(), line);
        }

        if (size.h.count() < 2)
            return;

        // Parse attributes. Be defensive: invalid/empty JSON ->
        // show a single "(no attributes)" line.
        std::vector<std::pair<std::string, std::string>> kvs;
        if (!s.attributes_json.empty()
            && s.attributes_json != "null") {
            try {
                auto j = nlohmann::json::parse(s.attributes_json);
                if (j.is_object()) {
                    for (auto it = j.begin(); it != j.end();
                         ++it) {
                        std::string v;
                        if (it->is_string())
                            v = it->get<std::string>();
                        else
                            v = it->dump();
                        kvs.emplace_back(it.key(), v);
                    }
                }
            } catch (...) {
            }
        }
        std::sort(
            kvs.begin(),
            kvs.end(),
            [](const auto & a, const auto & b) {
                return a.first < b.first;
            });

        auto rows_avail = size.h.count() - 1;
        auto cursor = nxt::Pos::origin() + 1 * nxt::ln;

        if (kvs.empty()) {
            auto r_size = nxt::Size{size.w, 1 * nxt::ln};
            auto r_sub = subraster(raster, cursor, r_size);
            std::ranges::fill(r_sub.fgs(), Rgba8{130, 140, 160});
            r_sub.write_text(
                nxt::Pos::origin(), " (no attributes)");
            return;
        }

        // Column allocation: key column is min(28, longest key + 2).
        std::size_t longest = 0;
        for (const auto & kv : kvs)
            longest = std::max(longest, kv.first.size());
        auto key_cells = std::min<std::size_t>(
            32, std::max<std::size_t>(12, longest + 2));
        auto val_cells = std::max<std::size_t>(
            8,
            static_cast<std::size_t>(size.w.count())
                - key_cells - 3);

        for (std::size_t i = 0;
             i < kvs.size()
             && static_cast<std::int64_t>(i) < rows_avail;
             ++i) {
            const auto & [k, v] = kvs[i];
            auto r_size = nxt::Size{size.w, 1 * nxt::ln};
            auto r_sub = subraster(raster, cursor, r_size);
            std::ranges::fill(
                r_sub.bgs(),
                (i & 1) ? Rgba8{18, 20, 28}
                        : Rgba8{14, 16, 22});

            auto key_str = fit(k, key_cells);
            r_sub.write_text(
                nxt::Pos::origin() + 1 * nxt::ch, key_str);
            for (std::size_t j = 0; j < key_cells; ++j)
                r_sub.set_fg(
                    nxt::Pos::origin()
                        + static_cast<int>(j + 1) * nxt::ch,
                    Rgba8{140, 180, 220});

            auto val_str = fit(v, val_cells);
            auto val_pos =
                nxt::Pos::origin()
                + static_cast<int>(key_cells + 2) * nxt::ch;
            r_sub.write_text(val_pos, val_str);
            for (std::size_t j = 0; j < val_cells; ++j)
                r_sub.set_fg(
                    val_pos + static_cast<int>(j) * nxt::ch,
                    Rgba8{225, 230, 240});

            cursor = cursor + 1 * nxt::ln;
        }
    }
};

// Compose the detail half: tree (flex) on top, attribute panel
// (fixed min, can grow) below.
struct DetailColumn
{
    std::shared_ptr<const Dataset> data;
    std::size_t trace_idx = 0;

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
        if (size.h.count() == 0)
            return;
        // Allocate ~60% of vertical space to the tree, ~40% to the
        // attribute panel. Adjust if either has natural content
        // smaller than its slot.
        auto h_total = size.h.count();
        auto attr_min = std::min<std::int64_t>(8, h_total / 2);
        auto attr_h = std::max<std::int64_t>(6,
            std::min<std::int64_t>(h_total / 2 + 2, h_total - 4));
        attr_h = std::min<std::int64_t>(attr_h, h_total);
        attr_h = std::max<std::int64_t>(attr_min, attr_h);
        auto tree_h = h_total - attr_h;
        if (tree_h < 4) {
            tree_h = h_total;
            attr_h = 0;
        }

        auto tree_size =
            nxt::Size{size.w, tree_h * nxt::ln};
        auto tree_sub = subraster(
            raster, nxt::Pos::origin(), tree_size);
        TraceDetailLayout{data, trace_idx}.render(
            tree_sub, tree_size);

        if (attr_h > 0) {
            auto attr_size =
                nxt::Size{size.w, attr_h * nxt::ln};
            auto attr_pos = nxt::Pos::origin()
                            + tree_h * nxt::ln;
            auto attr_sub = subraster(raster, attr_pos, attr_size);
            AttributePanel{data, trace_idx}.render(
                attr_sub, attr_size);
        }
    }
};

// ============================================================================
// Split-pane: fixed-width left, grow right, both fill available height
// ============================================================================

template<Layout L, Layout R>
struct SplitPane
{
    L left;
    R right;
    nxt::width_t left_width;

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
        if (size.h.count() == 0)
            return;
        auto lw = std::min(left_width, size.w);
        auto rw = size.w - lw;
        if (lw.count() > 0) {
            auto ls = nxt::Size{lw, size.h};
            auto sub =
                subraster(raster, nxt::Pos::origin(), ls);
            left.render(sub, ls);
        }
        if (rw.count() > 0) {
            auto rs = nxt::Size{rw, size.h};
            auto sub = subraster(
                raster, nxt::Pos::origin() + lw, rs);
            right.render(sub, rs);
        }
    }
};

template<Layout L, Layout R>
auto split_pane(L && l, R && r, nxt::width_t left_w)
{
    return SplitPane<std::decay_t<L>, std::decay_t<R>>{
        std::forward<L>(l),
        std::forward<R>(r),
        left_w,
    };
}

// ============================================================================
// Full-screen wrapper: split-pane fills available area, status bar on
// the last line. Reports flex>0 so the runtime enters fullscreen mode.
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

// ============================================================================
// Root body
// ============================================================================

struct UiState
{
    std::shared_ptr<Dataset> data;
    std::size_t selected = 0;

    void redraw(yard & self)
    {
        constexpr auto list_width = 44 * nxt::ch;
        auto list = TraceListLayout{
            std::span<const Trace>(data->traces),
            selected,
        };
        auto detail = DetailColumn{
            data,
            selected,
        };
        self.draw(screen(
            split_pane(
                std::move(list),
                std::move(detail),
                list_width),
            status_bar(selected, data->traces.size())));
    }
};

inline nxt::task<>
root(yard & self, std::shared_ptr<Dataset> data)
{
    if (data->traces.empty()) {
        self.println("(no traces found in dataset)");
        co_await next_key_press(self, is_quit_key);
        co_return;
    }

    UiState ui{std::move(data), 0};
    ui.redraw(self);

    while (!self.cancelled()) {
        auto event = co_await self.next_input();
        if (!event)
            co_return;
        if (event->type == nxt::input::EventType::release)
            continue;
        if (is_quit_key(*event))
            co_return;

        // Page step is rough — we don't know the pane height
        // outside of render; use terminal height as a proxy.
        auto page_step = std::max<std::size_t>(
            1,
            static_cast<std::size_t>(
                self.runtime().terminal_height().count())
                / 2);

        bool changed = false;
        if (event->key == nxt::input::Key::down
            || is_character(*event, 'j')) {
            if (ui.selected + 1 < ui.data->traces.size()) {
                ++ui.selected;
                changed = true;
            }
        } else if (
            event->key == nxt::input::Key::up
            || is_character(*event, 'k')) {
            if (ui.selected > 0) {
                --ui.selected;
                changed = true;
            }
        } else if (
            event->key == nxt::input::Key::page_down) {
            ui.selected = std::min<std::size_t>(
                ui.selected + page_step,
                ui.data->traces.size() - 1);
            changed = true;
        } else if (event->key == nxt::input::Key::page_up) {
            ui.selected =
                ui.selected > page_step
                    ? ui.selected - page_step
                    : 0;
            changed = true;
        } else if (
            event->key == nxt::input::Key::home
            || is_character(*event, 'g')) {
            ui.selected = 0;
            changed = true;
        } else if (
            event->key == nxt::input::Key::end
            || is_character(*event, 'G')) {
            ui.selected = ui.data->traces.size() - 1;
            changed = true;
        }

        if (changed)
            ui.redraw(self);
    }
}

} // namespace nxt::span_browser

int main(int argc, char ** argv)
{
    using namespace nxt::span_browser;

    auto path = std::string{
        argc > 1 ? argv[1] : "/home/mbrock/otel-archive"};

    std::cerr << "Loading spans from " << path << " ...\n";
    auto load_start = std::chrono::steady_clock::now();
    auto spans_result = load_spans(path);
    if (!spans_result.ok()) {
        std::cerr << "error: " << spans_result.status().ToString()
                  << "\n";
        return 1;
    }
    auto spans = std::move(spans_result).MoveValueUnsafe();
    auto load_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - load_start)
            .count();
    std::cerr << "  " << spans.size() << " spans loaded in "
              << load_elapsed << " ms\n";

    auto traces = assemble_traces(spans);
    std::cerr << "  " << traces.size()
              << " traces assembled (newest first)\n";

    auto data = std::make_shared<Dataset>(
        Dataset{std::move(spans), std::move(traces)});

    return nxt::ui::run2(
        [data = std::move(data)](yard & self) -> nxt::task<> {
            co_await nxt::span_browser::root(self, data);
        });
}
