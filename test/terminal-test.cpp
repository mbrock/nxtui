#include "vterm-wrapper.hpp"
#include <nxt/ansi.hpp>
#include <nxtio/app.hpp>
#include <nxt/tui.hpp>

#include <boost/ut.hpp>
#include <format>
#include <sstream>

namespace nxt::test {

using namespace boost::ut;
namespace tui = nxt::tui;

// ============================================================================
// Test helpers
// ============================================================================

/// Render a layout through the compositor and capture ANSI output.
std::string render_to_string(
    ui::TerminalCompositor & compositor, const auto & layout, Size size)
{
    ansi::mode = ansi::Mode::enabled;
    auto & buffer = compositor.back_buffer();
    buffer.clear();
    auto view = buffer.view();
    layout.render(view, size);

    std::ostringstream out;
    compositor.present_frame(out);
    return out.str();
}

/// Apply compositor control sequences to the virtual terminal.
void set_hud_height(
    ui::TerminalCompositor & compositor,
    vterm::Terminal & term,
    height_t hud_height,
    height_t term_height)
{
    ansi::mode = ansi::Mode::enabled;
    std::ostringstream out;
    compositor.set_hud_height(hud_height, term_height, out);
    term.write(out.str());
}

/// Check terminal display matches expected rows (visual test pattern).
/// Trailing spaces are trimmed for comparison.
void check_display(
    vterm::Terminal & term, std::vector<std::string> expected)
{
    for (std::size_t i = 0; i < expected.size(); ++i) {
        auto actual = term.get_row_text(static_cast<int>(i));
        while (!actual.empty() && actual.back() == ' ')
            actual.pop_back();
        expect(actual == expected[i])
            << std::format("row {}: '{}' != '{}'", i, actual, expected[i]);
    }
}

std::string separator_row(std::size_t columns)
{
    std::string result;
    for (std::size_t i = 0; i < columns; ++i)
        result += "▔";
    return result;
}

void write_at(vterm::Terminal & term, row_t row, std::string_view text)
{
    ansi::mode = ansi::Mode::enabled;
    std::string buf;
    nxt::ansi::Writer w(buf);
    w.move_to(Pos{terminal_origin + 0 * ch, row});
    w.text(text);
    term.write(buf);
}

/// Emit a line at the scroll region bottom (simulates println).
void println_at(vterm::Terminal & term, row_t row, std::string_view text)
{
    ansi::mode = ansi::Mode::enabled;
    std::string buf;
    nxt::ansi::Writer w(buf);
    w.move_to(Pos{terminal_origin + 0 * ch, row});
    w.text(text);
    w.clear_line_from_cursor();
    w.text("\n");
    term.write(buf);
}

// ============================================================================
// Compositor tests
// ============================================================================

suite compositor_tests = [] {
    "renders text at correct position"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 5 * ln}, glyphs);
        vterm::Terminal term(5, 20);

        auto output = render_to_string(
            compositor, tui::text("Hello"), {20 * ch, 5 * ln});
        term.write(output);

        check_display(
            term,
            {
                "Hello",
                "",
                "",
                "",
                "",
            });
    };

    "renders column layout"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 5 * ln}, glyphs);
        vterm::Terminal term(5, 20);

        auto layout = tui::column(
            tui::text("One"), tui::text("Two"), tui::text("Three"));
        term.write(render_to_string(compositor, layout, {20 * ch, 5 * ln}));

        check_display(
            term,
            {
                "One",
                "Two",
                "Three",
                "",
                "",
            });
    };

    "renders styled text with colors"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 1 * ln}, glyphs);
        vterm::Terminal term(1, 20);

        const Rgba8 red(255, 0, 0);
        term.write(render_to_string(
            compositor, tui::text("Red", tui::fg(red)), {20 * ch, 1 * ln}));

        auto cell = term.get_cell(0, 0);
        expect(cell.has_value());
        expect(cell->fg.is_rgb() && cell->fg.c.rgb.red == 255) << "is red";
    };

    "renders bold text"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 1 * ln}, glyphs);
        vterm::Terminal term(1, 20);

        term.write(render_to_string(
            compositor, tui::text("Bold", tui::bold), {20 * ch, 1 * ln}));

        auto cell = term.get_cell(0, 0);
        expect(cell.has_value() && cell->bold);
    };

    "combines emphasis styles"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 1 * ln}, glyphs);
        vterm::Terminal term(1, 20);

        term.write(render_to_string(
            compositor,
            tui::text("Both", tui::bold | tui::underline),
            {20 * ch, 1 * ln}));

        auto cell = term.get_cell(0, 0);
        expect(cell.has_value());
        expect(cell->bold);
        expect(cell->underline);
    };
};

// ============================================================================
// HUD mode tests
// ============================================================================

suite hud_tests = [] {
    "HUD appears at bottom of terminal"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 6 * ln}, glyphs);
        vterm::Terminal term(6, 20);

        set_hud_height(compositor, term, 2 * ln, 6 * ln);

        auto layout =
            tui::column(tui::text("HUD-LINE-1"), tui::text("HUD-LINE-2"));
        term.write(render_to_string(compositor, layout, {20 * ch, 2 * ln}));

        // clang-format off
    check_display (term, {
      "",            // row 0 ─┐
      "",            // row 1  │ scroll
      "",            // row 2  │ region
      separator_row(20), // row 3 separator
      "HUD-LINE-1",  // row 4 ─┐ HUD
      "HUD-LINE-2",  // row 5 ─┘
    });
        // clang-format on
    };

    "full screen mode when HUD equals terminal height"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 3 * ln}, glyphs);
        vterm::Terminal term(3, 20);

        set_hud_height(compositor, term, 3 * ln, 3 * ln);

        auto layout =
            tui::column(tui::text("A"), tui::text("B"), tui::text("C"));
        term.write(render_to_string(compositor, layout, {20 * ch, 3 * ln}));

        check_display(term, {"A", "B", "C"});
    };

    "shrinking HUD clears freed rows"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 6 * ln}, glyphs);
        vterm::Terminal term(6, 20);

        set_hud_height(compositor, term, 4 * ln, 6 * ln);

        auto tall_hud = tui::column(
            tui::text("OLD-1"),
            tui::text("OLD-2"),
            tui::text("OLD-3"),
            tui::text("OLD-4"));
        term.write(
            render_to_string(compositor, tall_hud, {20 * ch, 4 * ln}));

        check_display(
            term,
            {
                "",
                separator_row(20),
                "OLD-1",
                "OLD-2",
                "OLD-3",
                "OLD-4",
            });

        set_hud_height(compositor, term, 2 * ln, 6 * ln);

        auto short_hud =
            tui::column(tui::text("NEW-1"), tui::text("NEW-2"));
        term.write(
            render_to_string(compositor, short_hud, {20 * ch, 2 * ln}));

        check_display(
            term,
            {
                "",
                "",
                "",
                separator_row(20),
                "NEW-1",
                "NEW-2",
            });
    };

    "growing HUD preserves bottom log content"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 6 * ln}, glyphs);
        vterm::Terminal term(6, 20);

        set_hud_height(compositor, term, 2 * ln, 6 * ln);
        write_at(term, terminal_origin_v + 2 * ln, "BOTTOM");

        set_hud_height(compositor, term, 3 * ln, 6 * ln);

        check_display(
            term,
            {
                "",
                "BOTTOM",
                "",
                "",
                "",
                "",
            });
    };

    "shrinking HUD moves log content into freed rows"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 6 * ln}, glyphs);
        vterm::Terminal term(6, 20);

        set_hud_height(compositor, term, 3 * ln, 6 * ln);
        write_at(term, terminal_origin_v + 1 * ln, "BOTTOM");

        set_hud_height(compositor, term, 2 * ln, 6 * ln);

        check_display(
            term,
            {
                "",
                "",
                "BOTTOM",
                "",
                "",
                "",
            });
    };
};

// ============================================================================
// Scroll region + HUD interaction
// ============================================================================

suite scroll_region_tests = [] {
    "println scrolls content without affecting HUD"_test = [] {
        // 6 row terminal, 2 row HUD at bottom
        // Scroll region: rows 0-2, separator: row 3, HUD: rows 4-5
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 6 * ln}, glyphs);
        vterm::Terminal term(6, 20);

        set_hud_height(compositor, term, 2 * ln, 6 * ln);

        // Render HUD
        auto hud =
            tui::column(tui::text("HUD-LINE-1"), tui::text("HUD-LINE-2"));
        term.write(render_to_string(compositor, hud, {20 * ch, 2 * ln}));

        // Set scroll region
        std::string buf;
        nxt::ansi::Writer sw(buf);
        sw.set_scroll_region(
            terminal_origin_v + 0 * ln, terminal_origin_v + 2 * ln);
        term.write(buf);

        // Initial state
        // clang-format off
    check_display (term, {
      "",            // row 0 ─┐
      "",            // row 1  │ scroll
      "",            // row 2  │ region
      separator_row(20), // row 3 separator
      "HUD-LINE-1",  // row 4 ─┐ HUD
      "HUD-LINE-2",  // row 5 ─┘
    });
        // clang-format on

        // First log line
        println_at(term, terminal_origin_v + 2 * ln, "LOG-1");

        // clang-format off
    check_display (term, {
      "",            // row 0
      "LOG-1",       // row 1 <- scrolled up from row 2
      "",            // row 2
      separator_row(20), // row 3
      "HUD-LINE-1",  // row 4  HUD unchanged
      "HUD-LINE-2",  // row 5
    });
        // clang-format on

        // Second log line
        println_at(term, terminal_origin_v + 2 * ln, "LOG-2");

        // clang-format off
    check_display (term, {
      "LOG-1",       // row 0 <- scrolled up again
      "LOG-2",       // row 1
      "",            // row 2
      separator_row(20), // row 3
      "HUD-LINE-1",  // row 4  HUD still unchanged
      "HUD-LINE-2",  // row 5
    });
        // clang-format on

        // Third log line - oldest visible line scrolls off, bottom stays blank
        println_at(term, terminal_origin_v + 2 * ln, "LOG-3");

        // clang-format off
    check_display (term, {
      "LOG-2",       // row 0
      "LOG-3",       // row 1
      "",            // row 2
      separator_row(20), // row 3
      "HUD-LINE-1",  // row 4  HUD still unchanged!
      "HUD-LINE-2",  // row 5
    });
        // clang-format on
    };
};

// ============================================================================
// Diff rendering tests
// ============================================================================

suite diff_tests = [] {
    "only changed cells are re-rendered"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({10 * ch, 1 * ln}, glyphs);

        auto output1 = render_to_string(
            compositor, tui::text("AAAAAAAAAA"), {10 * ch, 1 * ln});
        auto output2 = render_to_string(
            compositor, tui::text("AAABBBAAAA"), {10 * ch, 1 * ln});

        vterm::Terminal term(1, 10);
        term.write(output1);
        term.write(output2);

        expect(term.get_row_text(0) == "AAABBBAAAA");
        expect(output2.size() < output1.size()) << "diff is smaller";
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
