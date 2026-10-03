#include <nxtai/tool_tui.hpp>
#include <nxtai/trace_tui.hpp>
#include <nxtui/ansi.hpp>
#include <nxtui/raster-diff.hpp>
#include <nxtui/tui.hpp>
#include <nxtui/units.hpp>

#include "test.hpp"
#include <format>
#include <tuple>

namespace nxt::test {

using namespace nxtui;

using namespace boost::ut;
using nxtui::ChangeRun;
using nxtui::GlyphTable;
using nxtui::Raster;
using nxtui::Rgba8;

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

/// Render `layout` into a `size` raster and return its ASCII rows.
template<typename Layout>
std::vector<std::string> rendered_rows(const Layout & layout, Size size)
{
    GlyphTable glyphs;
    Raster raster(size.w, size.h, glyphs);
    auto view = raster.view();
    layout.render(view, size);

    auto rows = std::vector<std::string>{};
    for (std::size_t y = 0; y < size.h.count(); ++y) {
        auto & row = rows.emplace_back();
        for (std::size_t x = 0; x < size.w.count(); ++x)
            if (auto cell = view.get_cell(Pos::at(x * ch, y * ln)))
                row += static_cast<char>(cell->glyph);
    }
    return rows;
}

void expect_shows(
    const std::vector<std::string> & rows,
    std::string_view needle)
{
    auto shown = std::ranges::any_of(rows, [&](const std::string & row) {
        return row.contains(needle);
    });
    auto screen = std::string{};
    for (const auto & row : rows)
        screen += std::format("\n|{}|", row);
    expect(shown) << std::format("'{}' not shown in:{}", needle, screen);
}

// ============================================================================
// Layout tests
// ============================================================================

static suite layout_tests{
    "Layouts", [] {
        using namespace tui;

        "linear layouts"_group = [] {
            "column"_test = [] {
                renders(column(text("AAA"), text("BBB"), text("CCC")))
                    | "AAA" | "BBB" | "CCC";
            };

            "row with fill"_test = [] {
                renders(row(text("L"), fill(), text("R"))) | "L        R";
            };

            "row with multiple items"_test = [] {
                renders(row(text("A"), text(" "), text("B"))) | "A B";
            };
        };

        "text lines"_group = [] {
            "render multiple rows"_test = [] {
                auto layout = text_lines("alpha\nbeta");
                expect(layout.height_hint().min == 2 * ln);
                expect(layout.width_hint().min == 5 * ch);
                renders(layout) | "alpha" | "beta";
            };

            "preserve blank lines"_test = [] {
                auto layout = text_lines("alpha\n\nbeta");
                expect(layout.height_hint().min == 3 * ln);
                renders(layout) | "alpha" | "" | "beta";
            };

            "preserve spaces inside lines"_test = [] {
                renders(text_lines("alpha beta")) | "alpha beta";
            };

            "measure terminal cell width"_test = [] {
                auto layout = text_lines("\xe7\x95\x8c\nabc");
                expect(layout.width_hint().min == 3 * ch);
            };
        };

        "variants"_group = [] {
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

            "either builds only the chosen thunk"_test = [] {
                auto built = std::vector<std::string>{};
                auto layout = either(
                    true,
                    [&] {
                        built.push_back("off");
                        return text("off");
                    },
                    [&] {
                        built.push_back("on");
                        return text("on");
                    });

                expect(built == std::vector<std::string>{"on"});
                renders(layout) | "on";
            };

            "when is an optional typed child"_test = [] {
                auto hidden = when(false, text("hidden"));
                expect(hidden.width_hint().min == 0 * ch);
                expect(hidden.height_hint().min == 0 * ln);

                renders(column(text("top"), hidden, text("bottom")))
                    | "top" | "bottom";
                renders(when(true, text("visible"))) | "visible";
            };
        };

        "static shapes"_group = [] {
            "compositions keep their child types"_test = [] {
                using Text = decltype(text("x"));
                using Hidden = decltype(when(false, text("x")));
                static_assert(std::same_as<Hidden, Either<Empty, Text>>);
                static_assert(std::same_as<
                              decltype(row(text("a"), when(true, text("b")))),
                              Row<Text, Hidden>>);
                static_assert(std::same_as<
                              decltype(column(surface({}, text("a")))),
                              Column<Surface<Text>>>);
                static_assert(!std::convertible_to<Row<Text>, Text>);
            };

            "type erasure is opt-in at the boundary"_test = [] {
                auto erased = AnyLayout{row(text("L"), fill(), text("R"))};
                renders(erased) | "L        R";
            };
        };

        "runtime-sized children"_group = [] {
            "row of erased children"_test = [] {
                auto children = std::vector<AnyLayout>{};
                children.emplace_back(text("A"));
                children.emplace_back(fill());
                children.emplace_back(fixed_width(1 * ch, column(text("Z"))));
                renders(row(std::move(children))) | "A   Z";
            };

            "column of same-typed children"_test = [] {
                auto children = std::vector{text("one"), text("two")};
                auto layout = column(children);
                expect(layout.height_hint().min == 2 * ln);
                renders(layout) | "one" | "two";
            };

            "flex children share leftover space"_test = [] {
                auto layout = row(
                    std::vector{fill(), fill()});
                expect(layout.width_hint().flex == 2.0 * one);
                renders(row(text("["), fill(), text("|"), fill(), text("]")))
                    | "[   |   ]";
            };
        };

        "data views"_group = [] {
            "each maps borrowed items to variable-height layouts"_test = [] {
                auto items = std::vector<std::string>{"a\nb", "c"};
                auto layout = each(items, [](const std::string & item) {
                    return text_lines(item);
                });

                expect(layout.height_hint().min == 3 * ln);
                expect(layout.width_hint().min == 1 * ch);
                renders(layout) | "a" | "b" | "c";
            };
        };

        "tool views"_group = [] {
            "render a finished generic call with its result"_test = [] {
                namespace tt = nxtai::tool_tui;
                auto turn = tt::turn_view{
                    .thought = {},
                    .calls = {tt::call_view{
                        .name = "read_file",
                        .arguments = "src/main.cpp",
                        .output = "int main() {}",
                        .latest_memory_current = std::nullopt,
                        .state = tt::status::ok,
                        .elapsed_ms = 12,
                    }},
                };
                auto rows =
                    rendered_rows(tt::render_turn(turn), {60 * ch, 3 * ln});

                expect_shows(rows, " ok  file src/main.cpp");
                expect_shows(rows, " 12ms  13B ");
                expect_shows(rows, "int main() {}");
            };

            "render a running multi-line shell script"_test = [] {
                namespace tt = nxtai::tool_tui;
                auto call = tt::call_view{
                    .name = "bash",
                    .arguments = R"({"command":"cd src\nls"})",
                    .output = {},
                    .latest_memory_current = 2048,
                    .state = tt::status::running,
                    .elapsed_ms = -1,
                };
                auto rows =
                    rendered_rows(tt::render_call(call), {60 * ch, 6 * ln});

                expect_shows(rows, R"({"command":"cd src\nls"})");
                expect_shows(rows, " 2K ");
                expect_shows(rows, "$ cd src");
                expect_shows(rows, "> ls");
                expect_shows(rows, "waiting for process output");
            };

            "render an empty span waterfall"_test = [] {
                namespace tr = nxtai::trace_tui;
                auto rows = rendered_rows(
                    tr::render_waterfall(tr::waterfall_view{
                        .subject = "request",
                        .total = std::chrono::milliseconds{5},
                        .rows = {},
                    }),
                    {60 * ch, 2 * ln});

                expect_shows(rows, " span request ");
                expect_shows(rows, "no completed child spans");
            };
        };

        "rules and styles"_group = [] {
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
    }};

// ============================================================================
// Glyph table tests
// ============================================================================

static suite glyph_table_tests{
    "Glyph table", [] {
        "keep owned lookup keys valid across arena growth"_test = [] {
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

        "restore ASCII entries on clear"_test = [] {
            GlyphTable glyphs;
            auto id = glyphs.intern("wide-glyph");
            expect(id >= 256_ul);
            glyphs.clear();

            expect(glyphs.size() == 256_ul);
            auto text = glyphs.get(static_cast<GlyphTable::GlyphId>('A'));
            expect(text && *text == std::string_view{"A"});
        };
    }};

// ============================================================================
// Text writing tests
// ============================================================================

static suite write_text_tests{
    "Text rasterization", [] {
        "store combining sequences as one glyph table entry"_test = [] {
            GlyphTable glyphs;
            Raster raster(3 * ch, 1 * ln, glyphs);
            auto view = raster.view();

            const std::string text = std::string{"e"} + "\xcc\x81" + "x";
            view.write_text(Pos::origin(), text);

            auto first = view.get_cell(Pos::origin());
            auto second = view.get_cell(Pos::at(1 * ch, 0 * ln));
            expect(first.has_value());
            expect(second.has_value());
            expect(
                glyphs[first->glyph]
                == std::string_view{text}.substr(0, 3));
            expect(glyphs[second->glyph] == std::string_view{"x"});
        };

        "mark wide glyph continuation cells without extra bytes"_test = [] {
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
            expect(
                glyphs[first->glyph]
                == std::string_view{text}.substr(0, 3));
            expect(glyphs[continuation->glyph] == std::string_view{});
            expect(glyphs[third->glyph] == std::string_view{"x"});
        };
    }};

// ============================================================================
// Diff algorithm tests
// ============================================================================

static suite raster_diff_tests{
    "Raster diffs", [] {
        "emit no diff for identical rasters"_test = [] {
            //  "    " -> "    " = no changes
            GlyphTable glyphs;
            Raster a(4 * ch, 1 * ln, glyphs);
            Raster b(4 * ch, 1 * ln, glyphs);

            int runs = 0;
            diff_rasters(a, b, [&](const ChangeRun &) { runs++; });
            expect(runs == 0_i);
        };

        "diff a single cell"_test = [] {
            //  "    " -> " X  " = one run
            GlyphTable glyphs;
            Raster a(4 * ch, 1 * ln, glyphs);
            Raster b(4 * ch, 1 * ln, glyphs);
            b.view().set_char(Pos::at(1 * ch, 0 * ln), 'X');

            std::vector<ChangeRun> runs;
            diff_rasters(
                a, b, [&](const ChangeRun & r) { runs.push_back(r); });

            expect(runs.size() == 1_ul);
            expect(runs[0].origin == Pos::at(1 * ch, 0 * ln));
        };

        "batch consecutive cell changes"_test = [] {
            //  "        " -> "  ABC   " = one run
            GlyphTable glyphs;
            Raster a(8 * ch, 1 * ln, glyphs);
            Raster b(8 * ch, 1 * ln, glyphs);
            b.view().write_text(Pos::at(2 * ch, 0 * ln), "ABC");

            std::vector<ChangeRun> runs;
            diff_rasters(
                a, b, [&](const ChangeRun & r) { runs.push_back(r); });

            expect(runs.size() == 1_ul);
            expect(runs[0].glyphs.size() == 3_ul);
        };

        "split at color boundaries"_test = [] {
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
            diff_rasters(
                a, b, [&](const ChangeRun & r) { runs.push_back(r); });

            expect(runs.size() == 2_ul);
            expect(runs[0].fg_change == red);
            expect(runs[1].fg_change == blue);
        };

        "diff multiple rows"_test = [] {
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
            diff_rasters(
                a, b, [&](const ChangeRun & r) { runs.push_back(r); });

            expect(runs.size() == 3_ul);
        };
    }};

// ============================================================================
// ANSI tests
// ============================================================================

static suite ansi_tests{
    "ANSI output", [] {
        "convert terminal coordinates to ANSI coordinates"_test = [] {
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

        "emit debug mode escapes"_test = [] {
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

        "emit synchronized update escapes"_test = [] {
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
    }};

} // namespace nxt::test
