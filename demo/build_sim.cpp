// Build simulator demo for the actor-style nxt UI.
//
// Each "derivation" is a coroutine that owns its own surface. Phase
// transitions (waiting on deps -> queued for a slot -> running -> done)
// happen in plain control flow inside the goal body, drawing into the
// goal's surface as it goes. The parent body just spawns goals,
// composes their surfaces in a column, and waits for `q`.
//
// There is no reducer, no event variant, no global UI state. The
// only shared things are real synchronization primitives: a semaphore
// per slot kind and one completion event per goal, all owned by a
// shared_ptr<SimContext> so they outlive root's stack and the
// published layout stays valid through the final render frame.

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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <numeric>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace nxt::build_sim {

using namespace nxt::tui;
using namespace nxt::ui;
using namespace std::chrono_literals;

// ============================================================================
// Synthetic derivation graph
// ============================================================================

struct Node
{
    std::string name;
    std::vector<std::size_t> inputs;
    std::chrono::milliseconds duration{0};
    bool substitute = false;

    // Display tree position (computed after the DAG is built).
    std::ptrdiff_t display_parent = -1;
    std::vector<std::size_t> display_children;
    int display_depth = 0;
};

struct Graph
{
    std::vector<Node> nodes;
    // Depth-first preorder over the display tree, starting from each
    // root. Used for ordered iteration and to propagate visibility
    // upward via reverse traversal.
    std::vector<std::size_t> display_dfs;
    // Deepest node in the display tree. The tree layout reserves
    // 2 * max_display_depth cells of prefix on every row so progress
    // bars line up across indentation depths.
    int max_display_depth = 0;
};

constexpr std::array<std::string_view, 48> stems = {
    "glibc",     "gcc",       "binutils",  "openssl",
    "zlib",      "pcre2",     "libxml2",   "libffi",
    "ncurses",   "readline",  "bash",      "coreutils",
    "findutils", "diffutils", "gawk",      "sed",
    "grep",      "tar",       "gzip",      "xz",
    "make",      "python3",   "perl",      "ruby",
    "go",        "rust",      "llvm",      "clang",
    "cmake",     "meson",     "ninja",     "ffmpeg",
    "libpng",    "libjpeg",   "freetype",  "fontconfig",
    "harfbuzz",  "cairo",     "pango",     "gtk4",
    "qt6",       "mesa",      "wayland",   "systemd",
    "dbus",      "udev",      "nss",       "sqlite",
};

constexpr std::array<std::string_view, 12> versions = {
    "1.0.0",  "2.3.1",  "0.42.0", "3.3.2",
    "1.3.1",  "10.44",  "5.2.21", "13.3.0",
    "2.40",   "6.7.4",  "0.18.2", "4.5.0",
};

inline Graph synthesize(std::uint32_t seed, std::size_t n_nodes)
{
    Graph g;
    g.nodes.reserve(n_nodes);

    std::mt19937 rng{seed};
    std::uniform_int_distribution<std::size_t> stem_d{
        0, stems.size() - 1};
    std::uniform_int_distribution<std::size_t> ver_d{
        0, versions.size() - 1};

    const auto n_layers = std::max<std::size_t>(
        3,
        static_cast<std::size_t>(std::sqrt((double) n_nodes)));

    std::vector<std::vector<std::size_t>> by_layer(n_layers);

    for (std::size_t i = 0; i < n_nodes; ++i) {
        auto layer = (i * n_layers) / n_nodes;
        if (layer >= n_layers)
            layer = n_layers - 1;

        Node node;
        node.name = std::format(
            "{}-{}",
            stems[stem_d(rng)],
            versions[ver_d(rng)]);

        if (layer > 0 && !by_layer[layer - 1].empty()) {
            auto pool = by_layer[layer - 1];
            std::shuffle(pool.begin(), pool.end(), rng);
            auto max_inputs =
                std::min<std::size_t>(4, pool.size());
            std::uniform_int_distribution<std::size_t> n_d{
                1, max_inputs};
            pool.resize(n_d(rng));
            node.inputs = std::move(pool);
        }

        node.substitute = (rng() % 100) < 65;

        if (node.substitute) {
            std::uniform_int_distribution<int> d{300, 1500};
            node.duration = std::chrono::milliseconds{d(rng)};
        } else {
            std::uniform_int_distribution<int> d{1400, 5500};
            node.duration = std::chrono::milliseconds{d(rng)};
        }

        g.nodes.push_back(std::move(node));
        by_layer[layer].push_back(i);
    }

    // Determine display roots: nodes that aren't anyone's input.
    std::vector<bool> is_input(n_nodes, false);
    for (const auto & n : g.nodes)
        for (auto inp : n.inputs)
            is_input[inp] = true;

    // DFS from each root into inputs (= display children). First
    // encounter wins, so each node appears in exactly one place in
    // the display tree even though the underlying DAG has shared
    // dependencies.
    std::vector<bool> visited(n_nodes, false);
    auto dfs = [&](auto & rec, std::size_t idx, int depth) -> void {
        if (visited[idx])
            return;
        visited[idx] = true;
        g.nodes[idx].display_depth = depth;
        g.display_dfs.push_back(idx);
        for (auto inp : g.nodes[idx].inputs) {
            if (!visited[inp]) {
                g.nodes[inp].display_parent =
                    static_cast<std::ptrdiff_t>(idx);
                g.nodes[idx].display_children.push_back(inp);
                rec(rec, inp, depth + 1);
            }
        }
    };
    for (std::size_t i = 0; i < n_nodes; ++i)
        if (!is_input[i])
            dfs(dfs, i, 0);

    // Pick up any orphans (cycles shouldn't happen in our DAG, but
    // defensive: nodes not reached by any root above).
    for (std::size_t i = 0; i < n_nodes; ++i)
        if (!visited[i])
            dfs(dfs, i, 0);

    for (const auto & n : g.nodes)
        if (n.display_depth > g.max_display_depth)
            g.max_display_depth = n.display_depth;

    return g;
}

// ============================================================================
// Runtime-sized counting semaphore with cancellable acquire
// ============================================================================

class Semaphore
{
public:
    explicit Semaphore(std::size_t count) : count_(count) {}

    nxt::task<bool> acquire(const yard & self)
    {
        while (count_ == 0) {
            if (self.cancelled())
                co_return false;
            co_await event_;
            event_.reset();
        }
        if (self.cancelled())
            co_return false;
        --count_;
        co_return true;
    }

    void release()
    {
        ++count_;
        event_.set();
    }

    // Wake every parked acquirer so they can observe cancellation.
    void wake_all()
    {
        event_.set();
    }

private:
    std::size_t count_;
    nxt::event event_;
};

// ============================================================================
// Shared simulation state, owned by shared_ptr so it survives both the
// goal coroutines and the published layout (which references it from
// the header leaf's capture). Outlives root's stack.
// ============================================================================

// Phase ranks. Larger = closer to the top of the sort. Goals only
// contribute a visible row in the active and lingering phases, but
// every phase has a distinct priority so the bubble sort always has
// well-defined comparisons.
constexpr int phase_gone = 0;
constexpr int phase_waiting = 1;
constexpr int phase_lingering = 2;
constexpr int phase_active = 3;

inline std::int64_t encode_priority(int phase)
{
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    // Pack: high bits = phase rank, low 50 bits = ms wall clock.
    // Within a phase, more recent transitions sort higher.
    return (static_cast<std::int64_t>(phase) << 50)
           | (ms & ((static_cast<std::int64_t>(1) << 50) - 1));
}

struct SimContext
{
    Graph graph;
    Semaphore build_slots;
    Semaphore subst_slots;
    std::vector<std::unique_ptr<nxt::event>> goal_complete;
    // Per-goal sort priority, updated at each phase transition.
    std::vector<std::atomic<std::int64_t>> priorities;
    // Display permutation: position -> handle index. Mutated by the
    // bubble-sort coroutine one swap at a time, read by DynColumn.
    std::vector<std::size_t> display_order;
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> active_builds{0};
    std::atomic<std::size_t> active_subs{0};
    std::size_t total;
    std::size_t build_cap;
    std::size_t subst_cap;

    SimContext(Graph g, std::size_t bcap, std::size_t scap)
        : graph(std::move(g))
        , build_slots(bcap)
        , subst_slots(scap)
        , priorities(graph.nodes.size())
        , display_order(graph.nodes.size())
        , total(graph.nodes.size())
        , build_cap(bcap)
        , subst_cap(scap)
    {
        goal_complete.reserve(total);
        for (std::size_t i = 0; i < total; ++i) {
            goal_complete.push_back(std::make_unique<nxt::event>());
            priorities[i].store(
                encode_priority(phase_waiting),
                std::memory_order_relaxed);
            display_order[i] = i;
        }
    }
};

// ============================================================================
// Dynamic column that owns its children and collapses around
// zero-height ones. Owns by value so the layout is self-contained.
// ============================================================================

template<Layout L>
struct DynColumn
{
    std::vector<L> children;
    // Optional permutation: when non-null, iterate children via
    // (*order)[pos] instead of pos. Updated externally (e.g. by a
    // sort coroutine running alongside the renderer on the same
    // scheduler thread).
    std::shared_ptr<const std::vector<std::size_t>> order;

    WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    HeightHint height_hint() const
    {
        nxt::height_t total{0 * nxt::ln};
        for (const auto & c : children)
            total = total + c.height_hint().min;
        return HeightHint::fixed(total);
    }

    void render(RasterView & raster, nxt::Size size) const
    {
        nxt::Pos cursor = nxt::Pos::origin();
        auto n = children.size();
        for (std::size_t pos = 0; pos < n; ++pos) {
            auto idx = order ? (*order)[pos] : pos;
            const auto & c = children[idx];
            auto h = c.height_hint().min;
            if (h.count() == 0)
                continue;
            if ((cursor.y - nxt::Pos::origin().y) + h > size.h)
                break;
            auto child_size = nxt::Size{size.w, h};
            auto sub = subraster(raster, cursor, child_size);
            c.render(sub, child_size);
            cursor = cursor + h;
        }
    }
};

template<Layout L>
auto dyn_column(
    std::vector<L> children,
    std::shared_ptr<const std::vector<std::size_t>> order = {})
{
    return DynColumn<L>{std::move(children), std::move(order)};
}

// ============================================================================
// FilteredDynColumn: same shape as DynColumn but with a per-child
// predicate. Three filtered columns sharing the same children vector
// and same sorted order let us split goals into NOM-style sections
// (Building / Downloading / Done) without each goal needing to know
// which section it belongs to — the predicate reads the goal's phase
// from ctx->priorities and decides.
// ============================================================================

template<Layout L>
struct FilteredDynColumn
{
    // Children are shared by all sections so we don't duplicate the
    // N AnyLayout surface handles per section.
    std::shared_ptr<const std::vector<L>> children;
    std::shared_ptr<const std::vector<std::size_t>> order;
    std::function<bool(std::size_t)> include;

    WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    HeightHint height_hint() const
    {
        nxt::height_t total{0 * nxt::ln};
        auto n = children->size();
        for (std::size_t pos = 0; pos < n; ++pos) {
            auto idx = order ? (*order)[pos] : pos;
            if (!include(idx))
                continue;
            total = total + (*children)[idx].height_hint().min;
        }
        return HeightHint::fixed(total);
    }

    void render(RasterView & raster, nxt::Size size) const
    {
        nxt::Pos cursor = nxt::Pos::origin();
        auto n = children->size();
        for (std::size_t pos = 0; pos < n; ++pos) {
            auto idx = order ? (*order)[pos] : pos;
            if (!include(idx))
                continue;
            const auto & c = (*children)[idx];
            auto h = c.height_hint().min;
            if (h.count() == 0)
                continue;
            if ((cursor.y - nxt::Pos::origin().y) + h > size.h)
                break;
            auto child_size = nxt::Size{size.w, h};
            auto sub = subraster(raster, cursor, child_size);
            c.render(sub, child_size);
            cursor = cursor + h;
        }
    }
};

template<Layout L>
auto filtered_dyn_column(
    std::shared_ptr<const std::vector<L>> children,
    std::shared_ptr<const std::vector<std::size_t>> order,
    std::function<bool(std::size_t)> include)
{
    return FilteredDynColumn<L>{
        std::move(children), std::move(order), std::move(include)};
}

// ============================================================================
// TreeLayout: renders goal surfaces in display-tree DFS order, with
// nix-output-monitor style tree characters (├ └ │) prefixed to each
// row. Visibility propagates upward from active/lingering goals so
// the ancestor path stays visible as context. Recomputed every frame
// from ctx->priorities (sub-millisecond for N=72), no caching.
// ============================================================================

struct TreeLayout
{
    std::shared_ptr<SimContext> ctx;
    std::shared_ptr<const std::vector<AnyLayout>> children;

    WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    HeightHint height_hint() const
    {
        auto vis = compute_visibility();
        nxt::height_t total{0 * nxt::ln};
        for (auto idx : vis.order) {
            auto h = (*children)[idx].height_hint().min;
            if (h.count() == 0)
                total = total + 1 * nxt::ln;
            else
                total = total + h;
        }
        return HeightHint::fixed(total);
    }

    void render(RasterView & raster, nxt::Size size) const
    {
        auto vis = compute_visibility();
        nxt::Pos cursor = nxt::Pos::origin();

        // Reserve a fixed-width prefix column based on the deepest
        // node in the whole graph, so the content area (and therefore
        // the progress bar) starts at the same screen column on every
        // row regardless of that row's actual depth.
        auto reserved_prefix_w =
            (ctx->graph.max_display_depth * 2) * nxt::ch;

        for (auto idx : vis.order) {
            auto surface_h = (*children)[idx].height_hint().min;
            // If the goal isn't drawing its own row (it's a context
            // ancestor with no surface content), reserve one line and
            // render just the tree position + name.
            bool is_context_only = surface_h.count() == 0;
            auto row_h =
                is_context_only ? 1 * nxt::ln : surface_h;

            if ((cursor.y - nxt::Pos::origin().y) + row_h > size.h)
                break;

            auto prefix_str = compute_prefix(idx, vis);

            // Paint prefix at the left, but reserve the full
            // max-depth width so content lines up. Cells past the
            // actual prefix length remain cleared (spaces).
            if (reserved_prefix_w.count() > 0) {
                auto prefix_size =
                    nxt::Size{reserved_prefix_w, row_h};
                auto prefix_sub =
                    subraster(raster, cursor, prefix_size);
                auto faint_fg = Rgba8{120, 130, 145};
                std::ranges::fill(prefix_sub.fgs(), faint_fg);
                prefix_sub.write_text(
                    nxt::Pos::origin(), prefix_str);
            }

            // Then paint the goal's row (or its name as context) to
            // the right of the reserved prefix column.
            auto content_pos = nxt::Pos{
                cursor.x + reserved_prefix_w, cursor.y};
            auto content_w = size.w - reserved_prefix_w;
            auto content_size = nxt::Size{content_w, row_h};
            auto content_sub =
                subraster(raster, content_pos, content_size);

            if (is_context_only) {
                auto name = ctx->graph.nodes[idx].name;
                auto name_fg = Rgba8{160, 165, 175};
                content_sub.write_text(
                    nxt::Pos::origin(), name);
                auto name_w = nxt::tui::utf8_width(name);
                for (auto x = nxt::Pos::origin().x;
                     x < nxt::Pos::origin().x + name_w;
                     x += 1 * nxt::ch)
                    content_sub.set_fg(
                        nxt::Pos{x, nxt::Pos::origin().y},
                        name_fg);
            } else {
                (*children)[idx].render(content_sub, content_size);
            }

            cursor = cursor + row_h;
        }
    }

private:
    struct VisState
    {
        std::vector<bool> visible;
        std::vector<bool> is_last_visible;
        std::vector<std::size_t> order;
    };

    VisState compute_visibility() const
    {
        VisState s;
        auto n = ctx->total;
        s.visible.assign(n, false);
        s.is_last_visible.assign(n, false);

        // Mark goals that have a visible row of their own.
        for (std::size_t i = 0; i < n; ++i) {
            auto p = ctx->priorities[i].load(
                std::memory_order_relaxed);
            auto phase = static_cast<int>(p >> 50);
            if (phase == phase_active
                || phase == phase_lingering)
                s.visible[i] = true;
        }

        // Propagate up: any ancestor of a visible node becomes
        // visible too. DFS preorder reversed = roughly post-order
        // for our purposes (children processed before parents in the
        // reverse direction).
        for (auto it = ctx->graph.display_dfs.rbegin();
             it != ctx->graph.display_dfs.rend(); ++it) {
            if (!s.visible[*it])
                continue;
            auto parent = ctx->graph.nodes[*it].display_parent;
            if (parent >= 0)
                s.visible[static_cast<std::size_t>(parent)] = true;
        }

        // Compute which visible child is the last visible one per
        // parent (for `└` vs `├`).
        for (std::size_t i = 0; i < n; ++i) {
            std::ptrdiff_t last = -1;
            for (auto c : ctx->graph.nodes[i].display_children) {
                if (s.visible[c])
                    last = static_cast<std::ptrdiff_t>(c);
            }
            if (last >= 0)
                s.is_last_visible[
                    static_cast<std::size_t>(last)] = true;
        }

        // Visible DFS order = full DFS filtered.
        for (auto i : ctx->graph.display_dfs)
            if (s.visible[i])
                s.order.push_back(i);

        return s;
    }

    std::string
    compute_prefix(std::size_t i, const VisState & vis) const
    {
        // Collect ancestors from this node up to root.
        std::vector<std::size_t> ancestors;
        auto cur = ctx->graph.nodes[i].display_parent;
        while (cur >= 0) {
            ancestors.push_back(static_cast<std::size_t>(cur));
            cur = ctx->graph.nodes[
                static_cast<std::size_t>(cur)].display_parent;
        }
        std::reverse(ancestors.begin(), ancestors.end());
        // ancestors now goes from root down to immediate parent.

        std::string out;
        // For ancestors above the immediate parent, draw vertical
        // continuation (`│ `) if that ancestor has more visible
        // siblings to come, or spaces if it was the last.
        for (std::size_t k = 1; k < ancestors.size(); ++k) {
            out += vis.is_last_visible[ancestors[k]]
                       ? "  "
                       : "│ ";
        }

        // This node's own marker — only if it has a parent.
        if (!ancestors.empty()) {
            out += vis.is_last_visible[i] ? "└ " : "├ ";
        }
        return out;
    }
};

inline TreeLayout tree_layout(
    std::shared_ptr<SimContext> ctx,
    std::shared_ptr<const std::vector<AnyLayout>> children)
{
    return TreeLayout{std::move(ctx), std::move(children)};
}

// ============================================================================
// Bottom-anchored layout: bot always gets its min height at the last
// rows, top gets whatever's left. Top is expected to self-truncate
// when it doesn't fit. Works around Column's lack of shrink semantics
// when sum-of-mins exceeds the allocated height.
// ============================================================================

template<Layout Top, Layout Bot>
struct BottomAnchor
{
    Top top;
    Bot bot;

    WidthHint width_hint() const
    {
        return WidthHint::grow();
    }

    HeightHint height_hint() const
    {
        // flex=0: stay a partial HUD so scrollback above remains
        // visible. The runtime treats any flex>0 as fullscreen.
        //
        // Clamp the reported height to a stable band: a small floor
        // so the HUD doesn't collapse to a single line of bar when
        // few goals are visible, and a ceiling so it doesn't grow
        // distractingly tall when many are active at once.
        constexpr auto floor = 8 * nxt::ln;
        constexpr auto ceiling = 24 * nxt::ln;
        auto natural =
            top.height_hint().min + bot.height_hint().min;
        auto clamped =
            natural < floor ? floor
            : natural > ceiling ? ceiling
                                : natural;
        return HeightHint::fixed(clamped);
    }

    void render(RasterView & raster, nxt::Size size) const
    {
        if (size.h.count() == 0)
            return;

        auto bot_h = std::min(bot.height_hint().min, size.h);
        auto top_h = size.h - bot_h;

        if (top_h.count() > 0) {
            auto top_size = nxt::Size{size.w, top_h};
            auto sub =
                subraster(raster, nxt::Pos::origin(), top_size);
            top.render(sub, top_size);
        }
        if (bot_h.count() > 0) {
            auto bot_size = nxt::Size{size.w, bot_h};
            auto bot_pos = nxt::Pos::origin() + top_h;
            auto sub = subraster(raster, bot_pos, bot_size);
            bot.render(sub, bot_size);
        }
    }
};

template<Layout Top, Layout Bot>
auto bottom_anchor(Top && top, Bot && bot)
{
    return BottomAnchor<std::decay_t<Top>, std::decay_t<Bot>>{
        std::forward<Top>(top),
        std::forward<Bot>(bot),
    };
}

// ============================================================================
// Perceptual color blending in OKLCH (ported from nixb's IdColor.hpp).
// Lightness lerps linearly; chroma uses a squared blend so colors
// desaturate faster than they darken — that's what makes a fade-out
// look right instead of going through muddy mid-tones.
// ============================================================================

inline std::tuple<float, float, float>
srgb_to_oklch(Rgba8 c)
{
    auto from_srgb = [](std::uint8_t v) {
        float x = static_cast<float>(v) / 255.0f;
        return x <= 0.04045f
                 ? x / 12.92f
                 : std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    float r = from_srgb(c.r());
    float g = from_srgb(c.g());
    float b = from_srgb(c.b());

    float X = 0.4124564f * r + 0.3575761f * g + 0.1804375f * b;
    float Y = 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
    float Z = 0.0193339f * r + 0.1191920f * g + 0.9503041f * b;

    float l = 0.8189330101f * X + 0.3618667424f * Y
              - 0.1288597137f * Z;
    float m = 0.0329845436f * X + 0.9293118715f * Y
              + 0.0361456387f * Z;
    float s = 0.0482003018f * X + 0.2643662691f * Y
              + 0.6338517070f * Z;

    float l_ = std::cbrt(std::max(l, 0.0f));
    float m_ = std::cbrt(std::max(m, 0.0f));
    float s_ = std::cbrt(std::max(s, 0.0f));

    float L = 0.2104542553f * l_ + 0.7936177850f * m_
              - 0.0040720468f * s_;
    float a = 1.9779984951f * l_ - 2.4285922050f * m_
              + 0.4505937099f * s_;
    float b2 = 0.0259040371f * l_ + 0.7827717662f * m_
               - 0.8086757660f * s_;

    return {L, std::sqrt(a * a + b2 * b2), std::atan2(b2, a)};
}

inline Rgba8 oklch_to_srgb(float L, float C, float h)
{
    float a_ = C * std::cos(h);
    float b_ = C * std::sin(h);

    float l_ = L + 0.3963377774f * a_ + 0.2158037573f * b_;
    float m_ = L - 0.1055613458f * a_ - 0.0638541728f * b_;
    float s_ = L - 0.0894841775f * a_ - 1.2914855480f * b_;

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    float X = 1.2270138511f * l - 0.5577999807f * m
              + 0.2812561490f * s;
    float Y = -0.0405801784f * l + 1.1122568696f * m
              - 0.0716766787f * s;
    float Z = -0.0763812845f * l - 0.4214819784f * m
              + 1.5861632204f * s;

    float r = 4.0767416621f * X - 3.3077115913f * Y
              + 0.2309699292f * Z;
    float g = -1.2684380046f * X + 2.6097574011f * Y
              - 0.3413193965f * Z;
    float b = -0.0041960863f * X - 0.7034186147f * Y
              + 1.7076147010f * Z;

    auto to_srgb = [](float c) -> std::uint8_t {
        c = std::clamp(c, 0.0f, 1.0f);
        float v = c <= 0.0031308f
                    ? c * 12.92f
                    : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return static_cast<std::uint8_t>(
            std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return Rgba8{to_srgb(r), to_srgb(g), to_srgb(b)};
}

// alpha = 1.0 returns `fg`, alpha = 0.0 returns `bg`.
inline Rgba8 blend_oklch(Rgba8 fg, Rgba8 bg, float alpha)
{
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    auto [L1, C1, h1] = srgb_to_oklch(fg);
    auto [L2, C2, h2] = srgb_to_oklch(bg);

    float L = L1 * alpha + L2 * (1.0f - alpha);

    // Square the chroma blend so colors desaturate quickly. Without
    // this, fades pass through over-saturated mid-tones and look bad.
    float alpha_c = alpha * alpha;
    float C = C1 * alpha_c + C2 * (1.0f - alpha_c);

    float h_diff = h2 - h1;
    if (h_diff > static_cast<float>(M_PI))
        h_diff -= 2.0f * static_cast<float>(M_PI);
    else if (h_diff < -static_cast<float>(M_PI))
        h_diff += 2.0f * static_cast<float>(M_PI);
    float h = h1 + h_diff * (1.0f - alpha);

    return oklch_to_srgb(L, C, h);
}

constexpr Rgba8 hud_bg{30, 34, 40};
constexpr Rgba8 done_color{120, 210, 130};
constexpr Rgba8 default_bar_bg{50, 50, 50};

// ============================================================================
// Fake output lines (just for visual texture)
// ============================================================================

// Build-output spam ported from nixb's sim-gen.hpp. Eight themed
// categories: real-feeling output from cmake, make, compilers,
// autoconf, cargo/rustc, python/setup.py, meson/ninja, plus an
// existential-comedy category. Each template's `{}` placeholders
// auto-consume the first N arguments from a fixed positional list
// (base name, version string, small int, mid int, big int, base+num,
// percent) — the substitutions occasionally get fed an off-type
// value, which is also part of the charm.

inline std::vector<std::string_view> const & cmake_spam()
{
    static const std::vector<std::string_view> v = {
        "-- The C compiler identification is GNU {}",
        "-- The CXX compiler identification is Clang {}",
        "-- Detecting C compiler ABI info - done",
        "-- Check for working C compiler: {} - skipped",
        "-- Detecting CXX compile features - done",
        "-- Found PkgConfig: /nix/store/{}",
        "-- Configuring done ({}s)",
        "-- Generating done ({}s)",
        "-- Build files have been written to: {}",
        "-- Looking for pthread.h",
        "-- Looking for pthread.h - found",
        "-- Performing Test CMAKE_HAVE_LIBC_PTHREAD",
        "-- Performing Test CMAKE_HAVE_LIBC_PTHREAD - Success",
        "-- Found Threads: TRUE",
        "-- Found ZLIB: /nix/store/{}/lib/libz.so (found version \"{}\")",
    };
    return v;
}

inline std::vector<std::string_view> const & make_spam()
{
    static const std::vector<std::string_view> v = {
        "make[{}]: Entering directory '{}'",
        "make[{}]: Leaving directory '{}'",
        "  CC       {}.o",
        "  CXX      {}.o",
        "  CCLD     {}",
        "  AR       lib{}.a",
        "  LINK     {}",
        "  GEN      {}",
        "  INSTALL  {}",
        "  STRIP    {}",
    };
    return v;
}

inline std::vector<std::string_view> const & compiler_spam()
{
    static const std::vector<std::string_view> v = {
        "gcc -c -O2 -fPIC -I/nix/store/{}/include -o {}.o {}.c",
        "g++ -std=c++20 -O2 -fPIC -I. -o {}.o {}.cpp",
        "clang++ -stdlib=libc++ -O3 -DNDEBUG -o {} {}.o",
        "ld -shared -o lib{}.so.{} {}.o",
        "ar rcs lib{}.a {}.o {}.o",
        "ranlib lib{}.a",
        "/nix/store/{}-binutils-wrapper-2.43.1/bin/ld: warning: {}",
    };
    return v;
}

inline std::vector<std::string_view> const & autoconf_spam()
{
    static const std::vector<std::string_view> v = {
        "checking for {}... yes",
        "checking for {}... no",
        "checking for {} in -l{}... yes",
        "checking whether {} works... yes",
        "checking size of {}... {}",
        "checking for {} support... experimental",
        "configure: creating ./config.status",
        "config.status: creating Makefile",
        "config.status: creating config.h",
        "config.status: executing libtool commands",
    };
    return v;
}

inline std::vector<std::string_view> const & rust_spam()
{
    static const std::vector<std::string_view> v = {
        "   Compiling {} v{} (/build/source)",
        "   Compiling {} v{}",
        "    Finished `release` profile [optimized] target(s) in {}s",
        "     Running `target/release/{}`",
        "    Building [===========>         ] {}/{}",
        "   Documenting {} v{}",
        "     Blocking waiting for file lock on package cache",
        "       Fresh {} v{}",
        "     warning: unused import: `{}`",
    };
    return v;
}

inline std::vector<std::string_view> const & python_spam()
{
    static const std::vector<std::string_view> v = {
        "running build",
        "running build_ext",
        "building '{}' extension",
        "creating build/temp.linux-x86_64-cpython-312",
        "gcc -fno-strict-overflow -Wsign-compare -DNDEBUG -g -O3 -Wall {}",
        "Successfully built {}",
        "Installing collected packages: {}",
        "Requirement already satisfied: {} in /nix/store/{}",
        "Collecting {} (from {})",
    };
    return v;
}

inline std::vector<std::string_view> const & meson_spam()
{
    static const std::vector<std::string_view> v = {
        "The Meson build system",
        "Version: {}",
        "Source dir: /build/source",
        "Build dir: /build/source/build",
        "Build type: native build",
        "Project name: {}",
        "Compiler for C supports link arguments -Wl,--as-needed: YES",
        "Dependency {} found: YES {}",
        "Build targets in project: {}",
        "ninja: Entering directory `/build/source/build'",
        "[{}/{}] Compiling C object {}",
        "[{}/{}] Linking target {}",
    };
    return v;
}

inline std::vector<std::string_view> const & existential_spam()
{
    static const std::vector<std::string_view> v = {
        "warning: {} is deprecated, but aren't we all?",
        "note: consider using `{}` instead... or don't, I'm not your mother",
        "info: reticulating splines for {}",
        "info: aligning cosmic bits in {}",
        "debug: {} has achieved enlightenment",
        "trace: {} ponders the void",
        "hint: have you tried turning {} off and on again?",
        "note: {} compiles, therefore it is",
        "warning: {} contains mass quantities of undefined behavior",
        "info: downloading more RAM for {}",
        "debug: teaching {} about the birds and the bees",
        "trace: {} is now sentient, send help",
    };
    return v;
}

inline std::vector<std::string_view> const & download_spam()
{
    static const std::vector<std::string_view> v = {
        "copying path '/nix/store/...-{}'",
        "  % Total    % Received  {}%",
        "downloading 'https://cache.nixos.org/nar/{}.nar.xz'",
        "  {} KiB / {} KiB  [{}%]",
        "fetching {} from binary cache...",
        "copying {} from 'https://cache.nixos.org'",
        "unpacking {} into /nix/store/...",
        "validating {} against NAR hash...",
        "decompressing {}.nar.xz ({} KiB)",
    };
    return v;
}

template<typename Container>
inline std::string_view
pick_template(std::mt19937 & rng, const Container & c)
{
    std::uniform_int_distribution<std::size_t> d{0, c.size() - 1};
    return c[d(rng)];
}

inline std::string
fake_build_line(std::mt19937 & rng, std::string_view name)
{
    // Pick one of eight themed categories at random.
    std::uniform_int_distribution<int> cat_d{0, 7};
    auto category = cat_d(rng);
    auto const & pool = category == 0 ? cmake_spam()
                      : category == 1 ? make_spam()
                      : category == 2 ? compiler_spam()
                      : category == 3 ? autoconf_spam()
                      : category == 4 ? rust_spam()
                      : category == 5 ? python_spam()
                      : category == 6 ? meson_spam()
                                      : existential_spam();
    auto tmpl = pick_template(rng, pool);

    // The seven positional args (auto-consumed in order by `{}`):
    // sometimes a template asks for what looks like a number and
    // gets `base` instead. The deliberate-but-uneven type fit is
    // part of the comedy of the original generator.
    std::string base{
        name.substr(0, std::min<std::size_t>(12, name.size()))};
    std::uniform_int_distribution<int> ver_a{1, 20};
    std::uniform_int_distribution<int> ver_b{0, 99};
    std::uniform_int_distribution<int> ver_c{0, 9};
    std::uniform_int_distribution<int> small{1, 4};
    std::uniform_int_distribution<int> mid{1, 999};
    std::uniform_int_distribution<int> big{10, 500};
    std::uniform_int_distribution<int> twoc{0, 99};
    std::uniform_int_distribution<int> pct{1, 100};

    auto version = std::format(
        "{}.{}.{}", ver_a(rng), ver_b(rng), ver_c(rng));
    auto small_n = small(rng);
    auto mid_n = mid(rng);
    auto big_n = big(rng);
    auto base_n = base + std::to_string(twoc(rng));
    auto pct_n = pct(rng);

    try {
        return std::vformat(
            tmpl,
            std::make_format_args(
                base, version, small_n, mid_n, big_n, base_n, pct_n));
    } catch (const std::format_error &) {
        return std::string{tmpl};
    }
}

inline std::string
fake_download_line(std::mt19937 & rng, std::string_view name)
{
    auto tmpl = pick_template(rng, download_spam());

    std::string base{
        name.substr(0, std::min<std::size_t>(16, name.size()))};
    std::uniform_int_distribution<int> sz{100, 50000};
    std::uniform_int_distribution<int> pc{0, 100};
    std::uniform_int_distribution<int> twoc{0, 99};

    auto kib = sz(rng);
    auto kib2 = sz(rng);
    auto percent = pc(rng);
    auto base_n = base + std::to_string(twoc(rng));

    try {
        return std::vformat(
            tmpl,
            std::make_format_args(
                base, kib, kib2, percent, base_n));
    } catch (const std::format_error &) {
        return std::string{tmpl};
    }
}

// ============================================================================
// Layout helpers
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

constexpr std::array<std::string_view, 10> spinner_frames = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};

inline auto goal_row(
    std::string_view name,
    std::string_view detail,
    nxt::percent_t pct,
    bool substitute,
    int tick)
{
    auto color = substitute ? Rgba8::cyan() : Rgba8::yellow();
    auto frame = std::string{
        spinner_frames[tick % spinner_frames.size()]};
    return row(
        text(std::format(" {} ", frame), fg(color) | bold),
        text(fit(name, 24), fg(color)),
        text(std::string{"  "}),
        progress_bar(pct, color),
        text(std::format(" {:>3.0f}%", pct.value()),
             fg(color) | bold),
        text(std::string{"  "}),
        text(fit(detail, 22), faint));
}

inline auto done_row(
    std::string_view name,
    std::string_view detail,
    Rgba8 color,
    Rgba8 bar_bg)
{
    return row(
        text(std::string{" ✓ "}, fg(color) | bold),
        text(fit(name, 24), fg(color)),
        text(std::string{"  "}),
        progress_bar(100.0 * nxt::percent, color, bar_bg),
        text(std::string{" 100%"}, fg(color) | bold),
        text(std::string{"  "}),
        text(fit(detail, 22), fg(color)));
}

// Section headers (NOM style: a symbol, a label, a count). Computed
// live from ctx so the count tracks the current phase contents.
inline auto section_header(
    std::shared_ptr<const SimContext> ctx,
    std::string symbol,
    std::string label,
    Rgba8 symbol_color,
    std::function<bool(const SimContext &, std::size_t)> include)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx = std::move(ctx),
         symbol = std::move(symbol),
         label = std::move(label),
         symbol_color,
         include = std::move(include)](
            RasterView & r, nxt::Size) {
            std::size_t count = 0;
            for (std::size_t i = 0; i < ctx->total; ++i)
                if (include(*ctx, i))
                    ++count;

            std::ranges::fill(r.glyphs(), 32);
            std::ranges::fill(r.fgs(), DEFAULT_COLOR);
            std::ranges::fill(r.bgs(), DEFAULT_COLOR);
            std::ranges::fill(r.ems(), DEFAULT_EMPHASIS);

            auto pos = nxt::Pos::origin();
            auto sym = std::format(" {} ", symbol);
            auto sym_end = r.write_text(pos, sym);
            for (auto x = pos.x; x < sym_end; x += 1 * nxt::ch)
                r.set_fg(nxt::Pos{x, pos.y}, symbol_color);

            auto text = std::format("{} {}", label, count);
            auto text_end = r.write_text(
                nxt::Pos{sym_end, pos.y}, text);
            for (auto x = sym_end; x < text_end; x += 1 * nxt::ch) {
                r.set_fg(
                    nxt::Pos{x, pos.y}, Rgba8{180, 185, 195});
                r.set_em(nxt::Pos{x, pos.y}, Emphasis::bold);
            }
        });
}

inline auto header_row(std::shared_ptr<const SimContext> ctx)
{
    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * nxt::ln),
        [ctx = std::move(ctx)](RasterView & r, nxt::Size) {
            auto line = std::format(
                " build {:>3}/{:<3}    "
                "⚙ {}/{} active    "
                "↓ {}/{} subs    "
                "press q to quit",
                ctx->completed.load(),
                ctx->total,
                ctx->active_builds.load(),
                ctx->build_cap,
                ctx->active_subs.load(),
                ctx->subst_cap);
            std::ranges::fill(r.glyphs(), 32);
            std::ranges::fill(r.fgs(),
                              Rgba8{220, 220, 230});
            std::ranges::fill(r.bgs(), Rgba8{30, 34, 40});
            std::ranges::fill(r.ems(), DEFAULT_EMPHASIS);
            r.write_text(nxt::Pos::origin(), line);
        });
}

// ============================================================================
// Per-goal coroutine
// ============================================================================

inline nxt::task<> run_goal(
    yard & self,
    std::size_t idx,
    std::shared_ptr<SimContext> ctx)
{
    const auto & node = ctx->graph.nodes[idx];
    auto rng = std::mt19937{
        static_cast<std::uint32_t>(idx * 2654435761u + 17u)};

    auto set_phase = [&](int phase) {
        ctx->priorities[idx].store(
            encode_priority(phase), std::memory_order_relaxed);
    };

    // Phase 1: wait for input dependencies. The surface stays empty
    // here so the goal contributes no visible row while queued behind
    // its deps. set-on-cancel wakes us so we can observe the stop.
    for (auto dep : node.inputs) {
        if (self.cancelled())
            co_return;
        co_await *ctx->goal_complete[dep];
    }
    if (self.cancelled())
        co_return;

    // Phase 2: acquire a slot of the right kind.
    bool sub = node.substitute;
    auto & slots = sub ? ctx->subst_slots : ctx->build_slots;
    auto & counter = sub ? ctx->active_subs : ctx->active_builds;

    if (!co_await slots.acquire(self))
        co_return;
    ++counter;
    set_phase(phase_active);

    // Phase 3: simulate the work. Decouple frame rate (~30 fps) from
    // spinner rate (~80 ms/frame), detail-line refresh (~400 ms), and
    // scrollback log emission (every 600–1200 ms, randomized so
    // multiple goals don't lockstep), and derive elapsed time from
    // steady_clock so percent changes are smooth enough to exercise
    // the progress bar's 1/8-cell shading.
    const auto start = std::chrono::steady_clock::now();
    const auto duration = node.duration;
    constexpr auto frame_interval = 33ms;
    constexpr auto spinner_period = 80ms;
    constexpr auto detail_period = 400ms;
    auto next_detail = detail_period;

    std::uniform_int_distribution<int> log_jitter_d{600, 1200};
    auto next_log = std::chrono::milliseconds{log_jitter_d(rng)};

    std::string detail = sub ? fake_download_line(rng, node.name)
                             : fake_build_line(rng, node.name);

    while (!self.cancelled()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start);
        if (elapsed >= duration)
            break;

        if (elapsed >= next_detail) {
            detail = sub ? fake_download_line(rng, node.name)
                         : fake_build_line(rng, node.name);
            next_detail += detail_period;
        }

        if (elapsed >= next_log) {
            auto line = sub
                ? fake_download_line(rng, node.name)
                : fake_build_line(rng, node.name);
            self.println(std::format(
                "{:<24}  {}",
                fit(node.name, 24),
                line));
            next_log = elapsed
                       + std::chrono::milliseconds{
                           log_jitter_d(rng)};
        }

        auto pct = (100.0 * static_cast<double>(elapsed.count())
                    / static_cast<double>(duration.count()))
                   * nxt::percent;
        auto spinner_tick =
            static_cast<int>(elapsed / spinner_period);

        self.draw(goal_row(
            node.name, detail, pct, sub, spinner_tick));

        auto remaining = duration - elapsed;
        co_await self.sleep(std::min(frame_interval, remaining));
    }

    --counter;
    slots.release();

    if (self.cancelled())
        co_return;

    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

    // Wake dependents *now* so the next layer can start its slot
    // contention even while this row is still visibly lingering. The
    // linger is a visual flourish, not part of the dep schedule.
    ++ctx->completed;
    ctx->goal_complete[idx]->set();
    set_phase(phase_lingering);

    // Phase 4: linger with an OKLCH fade-out from done-green into the
    // HUD background. Per-frame redraw exercises the same 30 fps
    // cadence the work loop uses.
    constexpr auto linger_total = 1500ms;
    constexpr auto fade_frame = 33ms;
    auto linger_start = std::chrono::steady_clock::now();
    auto final_detail = std::format(
        "done {:.2f}s", elapsed_ms.count() / 1000.0);

    while (!self.cancelled()) {
        auto now = std::chrono::steady_clock::now();
        auto fade_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - linger_start);
        if (fade_elapsed >= linger_total)
            break;

        float t = static_cast<float>(fade_elapsed.count())
                  / static_cast<float>(linger_total.count());
        float alpha = 1.0f - t;
        auto fg_now = blend_oklch(done_color, hud_bg, alpha);
        auto bar_bg_now =
            blend_oklch(default_bar_bg, hud_bg, alpha);
        self.draw(done_row(
            node.name, final_detail, fg_now, bar_bg_now));
        co_await self.sleep(fade_frame);
    }

    // Phase 5: commit a permanent line to scrollback, then disappear
    // from the live region.
    self.println(std::format(
        " {}  {:<28}  {:>5.2f}s",
        sub ? "↓" : "✓",
        node.name,
        elapsed_ms.count() / 1000.0));
    self.draw(AnyLayout{});
    set_phase(phase_gone);
}

// ============================================================================
// Incremental bubble sort: one comparison-and-maybe-swap per tick.
// Runs alongside the goal coroutines and re-orders display_order in
// place. The "doing slowly what could be done fast" is the entire
// point — perception interprets discrete jumps as different items
// appearing, but a gradual swap carries identity through the motion.
// ============================================================================

inline nxt::task<>
bubble_sort_loop(yard & self, std::shared_ptr<SimContext> ctx)
{
    // Cocktail-shaker variant: alternate L->R and R->L passes so the
    // largest priority reaches position 0 just as fast as the
    // smallest reaches N-1. Plain bubble sort migrates one direction
    // per pass which is too slow when high-priority items are mixed
    // deep in the array.
    constexpr auto swap_pause = 12ms;
    constexpr auto settle_pause = 120ms;
    auto & order = ctx->display_order;

    auto compare_and_swap =
        [&](std::size_t lo, std::size_t hi) -> nxt::task<bool> {
            auto pa = ctx->priorities[order[lo]].load(
                std::memory_order_relaxed);
            auto pb = ctx->priorities[order[hi]].load(
                std::memory_order_relaxed);
            if (pa < pb) {
                std::swap(order[lo], order[hi]);
                self.runtime().signal_damage();
                co_await self.sleep(swap_pause);
                co_return true;
            }
            co_return false;
        };

    while (!self.cancelled()) {
        bool swapped = false;

        // Forward: smallest sinks to the right.
        for (std::size_t i = 0; i + 1 < order.size(); ++i) {
            if (self.cancelled())
                co_return;
            if (co_await compare_and_swap(i, i + 1))
                swapped = true;
        }
        // Backward: largest rises to the left.
        for (std::size_t i = order.size() - 1; i > 0; --i) {
            if (self.cancelled())
                co_return;
            if (co_await compare_and_swap(i - 1, i))
                swapped = true;
        }

        if (!swapped)
            co_await self.sleep(settle_pause);
    }
}

// ============================================================================
// Root body
// ============================================================================

inline nxt::task<> root(yard & self)
{
    constexpr std::size_t n_nodes = 72;
    constexpr std::size_t build_cap = 3;
    constexpr std::size_t subst_cap = 6;

    auto ctx = std::make_shared<SimContext>(
        synthesize(0xC0FFEEu, n_nodes), build_cap, subst_cap);

    std::vector<ProcessHandle> handles;
    handles.reserve(ctx->total);
    for (std::size_t i = 0; i < ctx->total; ++i) {
        handles.push_back(self.spawn(
            [i, ctx](yard & s) -> nxt::task<> {
                return run_goal(s, i, ctx);
            }));
    }

    // Build the published layout with owned data: render may run one
    // more frame after root returns, so nothing in the layout may
    // reference root's locals. The tree layout reads visibility and
    // structure from ctx each frame, and renders surfaces at their
    // display-tree positions.
    auto surfaces = std::make_shared<std::vector<AnyLayout>>();
    surfaces->reserve(handles.size());
    for (const auto & h : handles)
        surfaces->emplace_back(h.surface());

    self.draw(bottom_anchor(
        tree_layout(ctx, surfaces),
        header_row(ctx)));

    co_await next_key_press(self, is_quit_key);

    // Quit pressed. Cancel the scope, then wake everyone parked on a
    // synchronization primitive so they observe cancellation and
    // exit instead of deadlocking on a never-firing event.
    self.cancel();
    for (auto & ev : ctx->goal_complete)
        ev->set();
    ctx->build_slots.wake_all();
    ctx->subst_slots.wake_all();

    co_await self.scope().all();

    // Drop the live layout so render's last frame sees an empty cell
    // instead of a stale Column tree (defense in depth — the layout
    // is already self-contained, but this is cheap insurance).
    self.draw(AnyLayout{});
}

} // namespace nxt::build_sim

int main()
{
    return nxt::ui::run2(nxt::build_sim::root);
}
