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

suite layout_tests = [] {
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

suite glyph_table_tests = [] {
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
// Diff algorithm tests
// ============================================================================

suite diff_tests = [] {
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

suite ansi_tests = [] {
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

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
