#include "vterm-wrapper.hpp"
#include <nxt/ansi.hpp>
#include <nxtio/app.hpp>
#include <nxt/tui.hpp>
#include <nxt/tui_terminal.hpp>

#include <boost/ut.hpp>
#include <format>
#include <sstream>

namespace nxt::test {

using namespace boost::ut;
namespace tui = nxt::tui;

template<typename Exception, typename Fn>
bool throws_exception(Fn fn)
{
    try {
        fn();
    } catch (const Exception &) {
        return true;
    }
    return false;
}

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

void write_at(vterm::Terminal & term, row_t row, std::string_view text)
{
    ansi::mode = ansi::Mode::enabled;
    std::string buf;
    nxt::ansi::Writer w(buf);
    w.move_to(Pos{terminal_origin + 0 * ch, row});
    w.text(text);
    term.write(buf);
}

void move_cursor_to(vterm::Terminal & term, row_t row)
{
    ansi::mode = ansi::Mode::enabled;
    std::string buf;
    nxt::ansi::Writer w(buf);
    w.move_to(Pos{terminal_origin + 0 * ch, row});
    term.write(buf);
}

std::string apply_onlcr(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (auto ch : text) {
        if (ch == '\n')
            out.push_back('\r');
        out.push_back(ch);
    }
    return out;
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
    term.write(apply_onlcr(buf));
}

void block_at(
    vterm::Terminal & term,
    row_t row,
    std::string_view text,
    bool finish_at_next_line = true)
{
    ansi::mode = ansi::Mode::enabled;
    std::string buf;
    nxt::ansi::Writer w(buf);
    w.move_to(Pos{terminal_origin + 0 * ch, row});
    w.reset();
    w.text(text);
    if (finish_at_next_line)
        w.text("\n");
    term.write(apply_onlcr(buf));
}

void block_next(
    vterm::Terminal & term,
    std::string_view text,
    bool finish_at_next_line = true)
{
    ansi::mode = ansi::Mode::enabled;
    std::string buf;
    nxt::ansi::Writer w(buf);
    w.reset();
    w.text(text);
    if (finish_at_next_line)
        w.text("\n");
    term.write(apply_onlcr(buf));
}

void first_runtime_block_at(
    vterm::Terminal & term,
    row_t row,
    std::string_view text)
{
    ansi::mode = ansi::Mode::enabled;
    std::string buf;
    nxt::ansi::Writer w(buf);
    w.move_to(Pos{terminal_origin + 0 * ch, row});
    w.reset();
    w.text("\n");
    w.text(text);
    term.write(apply_onlcr(buf));
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

    "renders vterm screen layout"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({12 * ch, 3 * ln}, glyphs);
        vterm::Terminal source(3, 12);
        vterm::Terminal term(3, 12);

        source.write("\x1b[32mOK\x1b[0m\r\nplain");
        auto layout = tui::vterm_screen(source, tui::bg(Rgba8(10, 20, 30)));
        term.write(render_to_string(compositor, layout, {12 * ch, 3 * ln}));

        check_display(term, {"OK", "plain", ""});
        auto cell = term.get_cell(0, 0);
        expect(cell.has_value());
        expect(cell->fg.is_indexed() && cell->fg.c.indexed.idx == 2);
        auto blank = term.get_cell(2, 0);
        expect(blank.has_value());
        expect(blank->bg.is_rgb() && blank->bg.c.rgb.red == 10);
    };

    "renders vterm cursor overlay"_test = [] {
        GlyphTable glyphs;
        Raster raster({6 * ch, 2 * ln}, glyphs);
        vterm::Terminal source(2, 6);
        const auto pane_bg = Rgba8(10, 20, 30);
        auto layout =
            tui::vterm_screen(source, tui::bg(pane_bg) | tui::fg(Rgba8::white()));

        source.write("x");
        auto view = raster.view();
        layout.render(view, raster.extent());

        auto cursor = view.get_cell(Pos::at(1 * ch, 0 * ln));
        expect(cursor.has_value());
        expect(cursor->fg == pane_bg);
        expect(cursor->bg == Rgba8::white());

        source.write("\x1b[?25l");
        raster.clear();
        view = raster.view();
        layout.render(view, raster.extent());

        auto hidden = view.get_cell(Pos::at(1 * ch, 0 * ln));
        expect(hidden.has_value());
        expect(hidden->fg == Rgba8::white());
        expect(hidden->bg == pane_bg);
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

    "rejects control characters before terminal output"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 3 * ln}, glyphs);

        expect(throws_exception<std::logic_error>([&] {
            (void) render_to_string(
                compositor, tui::text("A\nB"), {20 * ch, 3 * ln});
        }));
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
      "",            // row 2  │
      "",            // row 3 ─┘
      "HUD-LINE-1",  // row 4 ─┐ HUD
      "HUD-LINE-2",  // row 5 ─┘
    });
        // clang-format on
    };

    "initial HUD install scrolls existing bottom prompt above HUD"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 12 * ln}, glyphs);
        vterm::Terminal term(12, 20);

        write_at(term, terminal_origin_v + 10 * ln, "PROMPT command");
        move_cursor_to(term, terminal_origin_v + 11 * ln);

        set_hud_height(compositor, term, 2 * ln, 12 * ln);

        auto layout =
            tui::column(tui::text("HUD-LINE-1"), tui::text("HUD-LINE-2"));
        term.write(render_to_string(compositor, layout, {20 * ch, 2 * ln}));

        check_display(
            term,
            {
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "PROMPT command",
                "HUD-LINE-1",
                "HUD-LINE-2",
            });
    };

    "initial HUD install leaves an empty first scrollback output row"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 12 * ln}, glyphs);
        vterm::Terminal term(12, 20);

        write_at(term, terminal_origin_v + 8 * ln, "PROMPT row 1");
        write_at(term, terminal_origin_v + 9 * ln, "PROMPT row 2");
        write_at(term, terminal_origin_v + 10 * ln, "PROMPT row 3");
        move_cursor_to(term, terminal_origin_v + 11 * ln);

        set_hud_height(compositor, term, 2 * ln, 12 * ln);

        first_runtime_block_at(
            term,
            terminal_origin_v
                + static_cast<std::size_t>(
                    compositor.scrollback_bottom_row())
                    * ln,
            "FIRST BLOCK");

        auto layout =
            tui::column(tui::text("HUD-LINE-1"), tui::text("HUD-LINE-2"));
        term.write(render_to_string(compositor, layout, {20 * ch, 2 * ln}));

        check_display(
            term,
            {
                "",
                "",
                "",
                "",
                "",
                "",
                "PROMPT row 1",
                "PROMPT row 2",
                "PROMPT row 3",
                "FIRST BLOCK",
                "HUD-LINE-1",
                "HUD-LINE-2",
            });
    };

    "initial HUD install and first block preserve wrapped shell command"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 6 * ln}, glyphs);
        vterm::Terminal term(6, 20);

        write_at(term, terminal_origin_v + 0 * ln, "a");
        write_at(term, terminal_origin_v + 1 * ln, "a");
        write_at(term, terminal_origin_v + 2 * ln, "XXXX");
        write_at(term, terminal_origin_v + 3 * ln, "YYYY");
        write_at(term, terminal_origin_v + 4 * ln, "ZZ");
        move_cursor_to(term, terminal_origin_v + 5 * ln);

        set_hud_height(compositor, term, 2 * ln, 6 * ln);
        expect(compositor.scrollback_bottom_row() == 3_i)
            << "scrollback_bottom_row is a zero-based row index";

        auto layout = tui::column(tui::text("1hud"), tui::text("2hud"));
        term.write(render_to_string(compositor, layout, {20 * ch, 2 * ln}));

        check_display(
            term,
            {
                "a",
                "XXXX",
                "YYYY",
                "ZZ",
                "1hud",
                "2hud",
            });

        first_runtime_block_at(
            term,
            terminal_origin_v
                + static_cast<std::size_t>(
                    compositor.scrollback_bottom_row())
                    * ln,
            "1txt\n2txt");

        check_display(
            term,
            {
                "YYYY",
                "ZZ",
                "1txt",
                "2txt",
                "1hud",
                "2hud",
            });
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
                "",
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
                "",
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

    "growing HUD preserves a box footer at the old scroll bottom"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 6 * ln}, glyphs);
        vterm::Terminal term(6, 20);

        set_hud_height(compositor, term, 1 * ln, 6 * ln);
        block_at(
            term,
            terminal_origin_v
                + static_cast<std::size_t>(
                    compositor.scrollback_bottom_row())
                    * ln,
            "╭──╮\n"
            "│A │\n"
            "╰──╯",
            false);

        set_hud_height(compositor, term, 2 * ln, 6 * ln);

        auto layout = tui::column(tui::text("HUD-1"), tui::text("HUD-2"));
        term.write(render_to_string(compositor, layout, {20 * ch, 2 * ln}));

        check_display(
            term,
            {
                "",
                "╭──╮",
                "│A │",
                "╰──╯",
                "HUD-1",
                "HUD-2",
            });
    };

    "entering HUD from zero preserves a box footer at the bottom"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 6 * ln}, glyphs);
        vterm::Terminal term(6, 20);

        set_hud_height(compositor, term, 0 * ln, 6 * ln);
        block_at(
            term,
            terminal_origin_v + 5 * ln,
            "╭──╮\n"
            "│A │\n"
            "╰──╯",
            false);

        set_hud_height(compositor, term, 2 * ln, 6 * ln);

        auto layout = tui::column(tui::text("HUD-1"), tui::text("HUD-2"));
        term.write(render_to_string(compositor, layout, {20 * ch, 2 * ln}));

        check_display(
            term,
            {
                "",
                "╭──╮",
                "│A │",
                "╰──╯",
                "HUD-1",
                "HUD-2",
            });
    };

    "output after shrinking HUD scrolls up to existing log content"_test = [] {
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
                "BOTTOM",
                "",
                "",
                "",
                "",
            });

        first_runtime_block_at(
            term,
            terminal_origin_v
                + static_cast<std::size_t>(
                    compositor.scrollback_bottom_row())
                    * ln,
            "NEXT");

        check_display(
            term,
            {
                "BOTTOM",
                "",
                "",
                "NEXT",
                "",
                "",
            });
    };

    "block output newline stacks next block without spacer row"_test = [] {
        GlyphTable glyphs;
        ui::TerminalCompositor compositor({20 * ch, 12 * ln}, glyphs);
        vterm::Terminal term(12, 20);

        set_hud_height(compositor, term, 2 * ln, 12 * ln);

        block_at(
            term,
            terminal_origin_v + 1 * ln,
            "╭────────╮\n"
            "│ first  │\n"
            "╰────────╯");
        block_next(
            term,
            "╭────────╮\n"
            "│ second │\n"
            "╰────────╯");

        check_display(
            term,
            {
                "",
                "╭────────╮",
                "│ first  │",
                "╰────────╯",
                "╭────────╮",
                "│ second │",
                "╰────────╯",
                "",
                "",
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
        // Scroll region: rows 0-3, HUD: rows 4-5
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
            terminal_origin_v + 0 * ln, terminal_origin_v + 3 * ln);
        term.write(buf);

        // Initial state
        // clang-format off
    check_display (term, {
      "",            // row 0 ─┐
      "",            // row 1  │ scroll
      "",            // row 2  │
      "",            // row 3 ─┘
      "HUD-LINE-1",  // row 4 ─┐ HUD
      "HUD-LINE-2",  // row 5 ─┘
    });
        // clang-format on

        // First log line
        println_at(term, terminal_origin_v + 3 * ln, "LOG-1");

        // clang-format off
    check_display (term, {
      "",            // row 0
      "",            // row 1
      "LOG-1",       // row 2 <- scrolled up from row 3
      "",            // row 3
      "HUD-LINE-1",  // row 4  HUD unchanged
      "HUD-LINE-2",  // row 5
    });
        // clang-format on

        // Second log line
        println_at(term, terminal_origin_v + 3 * ln, "LOG-2");

        // clang-format off
    check_display (term, {
      "",            // row 0
      "LOG-1",       // row 1 <- scrolled up again
      "LOG-2",       // row 2
      "",            // row 3
      "HUD-LINE-1",  // row 4  HUD still unchanged
      "HUD-LINE-2",  // row 5
    });
        // clang-format on

        // Third log line - oldest visible line scrolls off, bottom stays blank
        println_at(term, terminal_origin_v + 3 * ln, "LOG-3");

        // clang-format off
    check_display (term, {
      "LOG-1",       // row 0
      "LOG-2",       // row 1
      "LOG-3",       // row 2
      "",            // row 3
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
