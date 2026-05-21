#include <nxt/ansi.hpp>
#include <nxt/raster-diff.hpp>
#include <nxt/tui.hpp>
#include <nxt/units.hpp>

#include <boost/ut.hpp>
#include <format>

namespace nxt::test {

using namespace boost::ut;
using nxt::ChangeRun;
using nxt::GlyphTable;
using nxt::Raster;
using nxt::Rgba8;

// ============================================================================
// Test helper: renders(layout) | "row1" | "row2" | "row3";
// ============================================================================

template<typename Layout>
struct RenderChecker
{
    const Layout & layout;
    std::vector<std::string> expected;

    RenderChecker(const Layout & l)
        : layout(l)
    {
    }

    RenderChecker(RenderChecker &&) = default;
    RenderChecker & operator=(RenderChecker &&) = default;

    RenderChecker & operator|(std::string_view row)
    {
        expected.emplace_back(row);
        return *this;
    }

    ~RenderChecker()
    {
        if (expected.empty())
            return;

        std::size_t width = 0;
        for (const auto & row : expected)
            width = std::max(width, row.size());
        std::size_t height = expected.size();

        GlyphTable glyphs;
        Raster raster(width * ch, height * ln, glyphs);
        auto view = raster.view();
        layout.render(view, Size{width * ch, height * ln});

        for (std::size_t row_idx = 0; row_idx < expected.size();
             ++row_idx) {
            std::string actual;
            for (std::size_t x = 0; x < width; ++x) {
                auto cell = view.get_cell(Pos::at(x * ch, row_idx * ln));
                if (cell)
                    actual += static_cast<char>(cell->glyph);
            }
            while (!actual.empty() && actual.back() == ' ')
                actual.pop_back();

            expect(actual == expected[row_idx]) << std::format(
                "row {}: '{}' != '{}'", row_idx, actual, expected[row_idx]);
        }
    }
};

template<typename Layout>
RenderChecker<Layout> renders(const Layout & layout)
{
    return RenderChecker<Layout>{layout};
}

// ============================================================================
// Layout tests
// ============================================================================

static suite layout_tests = [] {
    using namespace tui;

    "column"_test = [] {
        renders(column(text("AAA"), text("BBB"), text("CCC"))) | "AAA"
            | "BBB" | "CCC";
    };

    "row with fill"_test = [] {
        renders(row(text("L"), fill(), text("R"))) | "L        R";
    };

    "row with multiple items"_test = [] {
        renders(row(text("A"), text(" "), text("B"))) | "A B";
    };

    "text_lines renders multiple rows"_test = [] {
        auto layout = text_lines("alpha\nbeta");
        expect(layout.height_hint().min == 2 * ln);
        expect(layout.width_hint().min == 5 * ch);
        renders(layout) | "alpha" | "beta";
    };

    "text_lines preserves blank lines"_test = [] {
        auto layout = text_lines("alpha\n\nbeta");
        expect(layout.height_hint().min == 3 * ln);
        renders(layout) | "alpha" | "" | "beta";
    };

    "text_lines preserves spaces inside lines"_test = [] {
        renders(text_lines("alpha beta")) | "alpha beta";
    };

    "text_lines measures terminal cell width"_test = [] {
        auto layout = text_lines("\xe7\x95\x8c\nabc");
        expect(layout.width_hint().min == 3 * ch);
    };

    "either renders selected branch"_test = [] {
        renders(either(false, text("off"), text("on"))) | "off";
        renders(either(true, text("off"), text("on"))) | "on";
    };

    "either uses selected branch hints"_test = [] {
        auto small = text("x");
        auto large = row(text("hello"), fill(), text("world"));

        auto false_selected = either(false, small, large);
        expect(false_selected.width_hint().min == 1 * ch);
        expect(false_selected.width_hint().flex == 0.0 * one);
        expect(false_selected.height_hint().min == 1 * ln);

        auto true_selected = either(true, small, large);
        expect(true_selected.width_hint().min == 10 * ch);
        expect(true_selected.width_hint().flex == 1.0 * one);
        expect(true_selected.height_hint().min == 1 * ln);
    };

    "hrule"_test = [] {
        GlyphTable glyphs;
        Raster raster(5 * ch, 1 * ln, glyphs);
        auto view = raster.view();
        hrule().render(view, Size{5 * ch, 1 * ln});

        int filled = 0;
        for (int x = 0; x < 5; ++x)
            if (auto cell = view.get_cell(Pos::at(x * ch, 0 * ln));
                cell && cell->glyph != ' ')
                filled++;
        expect(filled == 5_i) << "hrule fills width";
    };

    "style combines emphasis"_test = [] {
        const auto style = bold | underline;
        expect(has_emphasis(style.em, Emphasis::bold));
        expect(has_emphasis(style.em, Emphasis::underline));
    };
};

// ============================================================================
// Glyph table tests
// ============================================================================

static suite glyph_table_tests = [] {
    "owned lookup keys survive arena growth"_test = [] {
        GlyphTable glyphs;
        std::vector<GlyphTable::GlyphId> ids;
        std::vector<std::string> labels;

        for (int i = 0; i < 500; ++i) {
            labels.push_back(std::format("glyph-{}", i));
            ids.push_back(glyphs.intern(labels.back()));
        }

        for (std::size_t i = 0; i < labels.size(); ++i) {
            expect(glyphs.intern(labels[i]) == ids[i]);
            auto text = glyphs.get(ids[i]);
            expect(text && *text == std::string_view(labels[i]));
        }
    };

    "clear restores ASCII entries"_test = [] {
        GlyphTable glyphs;
        auto id = glyphs.intern("wide-glyph");
        expect(id >= 256_ul);
        glyphs.clear();

        expect(glyphs.size() == 256_ul);
        auto text = glyphs.get(static_cast<GlyphTable::GlyphId>('A'));
        expect(text && *text == std::string_view{"A"});
    };
};

// ============================================================================
// Text writing tests
// ============================================================================

static suite write_text_tests = [] {
    "combining sequence is one glyph table entry"_test = [] {
        GlyphTable glyphs;
        Raster raster(3 * ch, 1 * ln, glyphs);
        auto view = raster.view();

        const std::string text = std::string{"e"} + "\xcc\x81" + "x";
        view.write_text(Pos::origin(), text);

        auto first = view.get_cell(Pos::origin());
        auto second = view.get_cell(Pos::at(1 * ch, 0 * ln));
        expect(first.has_value());
        expect(second.has_value());
        expect(glyphs[first->glyph] == std::string_view{text}.substr(0, 3));
        expect(glyphs[second->glyph] == std::string_view{"x"});
    };

    "wide glyph marks continuation cell without extra bytes"_test = [] {
        GlyphTable glyphs;
        Raster raster(4 * ch, 1 * ln, glyphs);
        auto view = raster.view();

        const std::string text = std::string{"\xe7\x95\x8c"} + "x";
        auto end = view.write_text(Pos::origin(), text);

        auto first = view.get_cell(Pos::origin());
        auto continuation = view.get_cell(Pos::at(1 * ch, 0 * ln));
        auto third = view.get_cell(Pos::at(2 * ch, 0 * ln));
        expect(end == terminal_origin + 3 * ch);
        expect(first.has_value());
        expect(continuation.has_value());
        expect(third.has_value());
        expect(glyphs[first->glyph] == std::string_view{text}.substr(0, 3));
        expect(glyphs[continuation->glyph] == std::string_view{});
        expect(glyphs[third->glyph] == std::string_view{"x"});
    };
};

// ============================================================================
// Diff algorithm tests
// ============================================================================

static suite diff_tests = [] {
    "no diff when identical"_test = [] {
        //  "    " -> "    " = no changes
        GlyphTable glyphs;
        Raster a(4 * ch, 1 * ln, glyphs);
        Raster b(4 * ch, 1 * ln, glyphs);

        int runs = 0;
        diff_rasters(a, b, [&](const ChangeRun &) { runs++; });
        expect(runs == 0_i);
    };

    "single cell"_test = [] {
        //  "    " -> " X  " = one run
        GlyphTable glyphs;
        Raster a(4 * ch, 1 * ln, glyphs);
        Raster b(4 * ch, 1 * ln, glyphs);
        b.view().set_char(Pos::at(1 * ch, 0 * ln), 'X');

        std::vector<ChangeRun> runs;
        diff_rasters(a, b, [&](const ChangeRun & r) { runs.push_back(r); });

        expect(runs.size() == 1_ul);
        expect(runs[0].origin == Pos::at(1 * ch, 0 * ln));
    };

    "consecutive cells batch"_test = [] {
        //  "        " -> "  ABC   " = one run
        GlyphTable glyphs;
        Raster a(8 * ch, 1 * ln, glyphs);
        Raster b(8 * ch, 1 * ln, glyphs);
        b.view().write_text(Pos::at(2 * ch, 0 * ln), "ABC");

        std::vector<ChangeRun> runs;
        diff_rasters(a, b, [&](const ChangeRun & r) { runs.push_back(r); });

        expect(runs.size() == 1_ul);
        expect(runs[0].glyphs.size() == 3_ul);
    };

    "color boundary splits"_test = [] {
        //  "    " -> "AABB" (red, blue) = two runs
        GlyphTable glyphs;
        Raster a(4 * ch, 1 * ln, glyphs);
        Raster b(4 * ch, 1 * ln, glyphs);

        const Rgba8 red(255, 0, 0), blue(0, 0, 255);
        auto v = b.view();
        v.write_text(Pos::at(0 * ch, 0 * ln), "AA");
        v.set_fg(Pos::at(0 * ch, 0 * ln), red);
        v.set_fg(Pos::at(1 * ch, 0 * ln), red);
        v.write_text(Pos::at(2 * ch, 0 * ln), "BB");
        v.set_fg(Pos::at(2 * ch, 0 * ln), blue);
        v.set_fg(Pos::at(3 * ch, 0 * ln), blue);

        std::vector<ChangeRun> runs;
        diff_rasters(a, b, [&](const ChangeRun & r) { runs.push_back(r); });

        expect(runs.size() == 2_ul);
        expect(runs[0].fg_change == red);
        expect(runs[1].fg_change == blue);
    };

    "multiple rows"_test = [] {
        //  "    "      "A   "
        //  "    "  ->  "  B "
        //  "    "      "   C"
        GlyphTable glyphs;
        Raster a(4 * ch, 3 * ln, glyphs);
        Raster b(4 * ch, 3 * ln, glyphs);

        auto v = b.view();
        v.set_char(Pos::at(0 * ch, 0 * ln), 'A');
        v.set_char(Pos::at(2 * ch, 1 * ln), 'B');
        v.set_char(Pos::at(3 * ch, 2 * ln), 'C');

        std::vector<ChangeRun> runs;
        diff_rasters(a, b, [&](const ChangeRun & r) { runs.push_back(r); });

        expect(runs.size() == 3_ul);
    };
};

// ============================================================================
// ANSI tests
// ============================================================================

static suite ansi_tests = [] {
    "terminal to ANSI coords"_test = [] {
        // Terminal (0,0) -> ANSI (1,1)
        auto to_ansi_col = [](int t) {
            return (to_ansi(terminal_origin + t * ch) - ansi_origin)
                .count();
        };
        auto to_ansi_row = [](int t) {
            return (to_ansi(terminal_origin_v + t * ln) - ansi_origin_v)
                .count();
        };

        expect(to_ansi_col(0) == 1_i);
        expect(to_ansi_col(5) == 6_i);
        expect(to_ansi_row(0) == 1_i);
        expect(to_ansi_row(3) == 4_i);
    };

    "debug mode"_test = [] {
        auto saved = ansi::mode;
        ansi::mode = ansi::Mode::debug;

        std::string buf;
        ansi::Writer w(buf);
        w.move_to(Pos::at(5 * ch, 3 * ln));

        std::string_view out(buf);
        expect(out.find("⟨CSI:") != std::string_view::npos);
        expect(out.find("\x1b[") == std::string_view::npos);

        ansi::mode = saved;
    };

    "synchronized update codes"_test = [] {
        auto saved = ansi::mode;
        ansi::mode = ansi::Mode::enabled;

        std::string buf;
        ansi::Writer w(buf);
        w.begin_synchronized_update();
        w.end_synchronized_update();

        std::string_view out(buf);
        expect(out == std::string_view{"\x1b[?2026h\x1b[?2026l"});

        ansi::mode = saved;
    };
};

} // namespace nxt::test

