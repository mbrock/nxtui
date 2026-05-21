#include <boost/ut.hpp>

#include <nxt/raster.hpp>
#include <nxt/text_field.hpp>
#include <nxt/tui.hpp>
#include <nxt/units.hpp>
#include <nxt/utf8.hpp>
#include <nxtio/text_field.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

// ============================================================================
// Render helpers
// ============================================================================

struct RenderedRow
{
    std::string text;                       // glyphs as ASCII
    std::optional<std::size_t> cursor_cell; // first reverse cell
};

template<typename Layout>
RenderedRow render_row(const Layout & layout, std::size_t width)
{
    GlyphTable glyphs;
    Raster raster(width * ch, 1 * ln, glyphs);
    auto view = raster.view();
    layout.render(view, Size{width * ch, 1 * ln});

    RenderedRow out;
    for (std::size_t x = 0; x < width; ++x) {
        auto cell = view.get_cell(Pos::at(x * ch, 0 * ln));
        if (!cell)
            continue;
        out.text += static_cast<char>(cell->glyph);
        if (!out.cursor_cell.has_value()
            && has_emphasis(cell->em, Emphasis::reverse))
            out.cursor_cell = x;
    }
    return out;
}

// ============================================================================
// TextField (edit state)
// ============================================================================

static suite text_field_state{
    "Text field state", [] {
        using nxt::tui::TextField;
        using nxt::utf8::byte_offset;

        "insert at the end and advance the cursor"_test = [] {
            TextField f;
            f.insert("hi");
            expect(f.text == "hi") << "insert end text";
            expect(f.cursor_byte == byte_offset(2))
                << "insert end cursor=" << f.cursor_byte.count();
        };

        "insert in the middle and split text"_test = [] {
            TextField f{"abef", byte_offset(2)};
            f.insert("cd");
            expect(f.text == "abcdef");
            expect(f.cursor_byte == byte_offset(4));
        };

        "ignore erase-left at the start"_test = [] {
            TextField f;
            expect(!f.erase_left());
            expect(f.text.empty());
        };

        "erase one codepoint to the left"_test = [] {
            TextField f{"hello", byte_offset(5)};
            expect(f.erase_left());
            expect(f.text == "hell");
            expect(f.cursor_byte == byte_offset(4));
        };

        "erase UTF-8 to the left"_test = [] {
            TextField f{"å", byte_offset(2)}; // å is 2 bytes
            expect(f.erase_left());
            expect(f.text.empty());
            expect(f.cursor_byte == byte_offset(0));
        };

        "erase forward"_test = [] {
            TextField f{"abc", byte_offset(1)};
            expect(f.erase_right());
            expect(f.text == "ac");
            expect(f.cursor_byte == byte_offset(1));
        };

        "report false at movement edges"_test = [] {
            TextField f{"x", byte_offset(0)};
            expect(!f.move_left());
            f.cursor_byte = byte_offset(1);
            expect(!f.move_right());
        };

        "snap home and end to text boundaries"_test = [] {
            TextField f{"hello", byte_offset(3)};
            expect(f.move_home());
            expect(f.cursor_byte == byte_offset(0));
            expect(f.move_end());
            expect(f.cursor_byte == byte_offset(5));
        };

        "clear text and cursor state"_test = [] {
            TextField f{"hi", byte_offset(2)};
            f.clear();
            expect(f.text.empty());
            expect(f.cursor_byte == byte_offset(0));
        };

        "count cursor cells instead of bytes"_test = [] {
            TextField f{"å!", byte_offset(3)}; // 2-byte å then '!'
            expect(f.cursor_cell() == 2 * ch);
            expect(f.cell_count() == 2 * ch);
        };

        "use display width for cursor cells"_test = [] {
            TextField f{
                std::string{"\xe7\x95\x8c!"},
                byte_offset(4)}; // U+754C then '!'
            expect(f.cursor_cell() == 3 * ch);
            expect(f.cell_count() == 3 * ch);
        };

        "snap cursors inside UTF-8 to scalar boundaries"_test = [] {
            TextField f{"åx", byte_offset(1)}; // byte 1 is inside å
            expect(f.erase_right());
            expect(f.text == "x");
            expect(f.cursor_byte == byte_offset(0));
        };
    }};

// ============================================================================
// UTF-8 helpers
// ============================================================================

static suite utf8_helpers{
    "UTF-8 helpers", [] {
        using nxt::utf8::byte_offset;
        using nxt::utf8::grapheme_index;

        "move valid scalars as one cell"_test = [] {
            std::string text = "aå!";
            expect(nxt::utf8::count(text) == grapheme_index(3));
            expect(nxt::utf8::next(text, byte_offset(1)) == byte_offset(3));
            expect(nxt::utf8::prev(text, byte_offset(3)) == byte_offset(1));
            expect(
                nxt::utf8::cell_at(text, byte_offset(3))
                == grapheme_index(2));
        };

        "move combining sequences as one grapheme"_test = [] {
            std::string text = std::string{"e"} + "\xcc\x81" + "x";
            expect(nxt::utf8::count(text) == grapheme_index(2));
            expect(nxt::utf8::display_width(text) == 2 * ch);
            expect(nxt::utf8::next(text, byte_offset(0)) == byte_offset(3));
            expect(nxt::utf8::column_at(text, byte_offset(3)) == 1 * ch);
        };

        "measure wide scalars as two columns"_test = [] {
            std::string text{"\xe7\x95\x8c"};
            expect(nxt::utf8::display_width(text) == 2 * ch);
            expect(nxt::utf8::cluster_width(text) == 2 * ch);
        };

        "move malformed bytes one byte at a time"_test = [] {
            std::string text{
                "\xc0\xaf", 2}; // overlong slash, not valid UTF-8
            expect(nxt::utf8::count(text) == grapheme_index(2));
            expect(nxt::utf8::next(text, byte_offset(0)) == byte_offset(1));
            expect(nxt::utf8::next(text, byte_offset(1)) == byte_offset(2));
            expect(nxt::utf8::prev(text, byte_offset(2)) == byte_offset(1));
        };

        "view words with display widths"_test = [] {
            std::string text =
                " hello\twide \xe7\x95\x8c  e\xcc\x81\xc2\xa0"
                "bar";
            auto words = std::vector<nxt::utf8::word>{};
            for (auto word : nxt::utf8::words(text))
                words.push_back(word);

            expect(words.size() == std::size_t{5});
            expect(words[0].text == "hello");
            expect(words[0].width == 5 * ch);
            expect(words[2].text == "\xe7\x95\x8c");
            expect(words[2].width == 2 * ch);
            expect(words[3].text == "e\xcc\x81");
            expect(words[3].width == 1 * ch);
            expect(words[4].text == "bar");
        };

        "leave partial tails out of complete word prefixes"_test = [] {
            auto text = std::string{"hello wide wor"};
            auto prefix = nxt::utf8::complete_words_prefix_size(text);
            expect(prefix == std::size_t{11});

            auto complete = std::string_view{text}.substr(0, prefix);
            auto words = std::vector<std::string_view>{};
            for (auto word : nxt::utf8::words(complete))
                words.push_back(word.text);

            expect(words == std::vector<std::string_view>{"hello", "wide"});
        };

        "preserve line breaks and blank lines in segments"_test = [] {
            auto text = std::string{"hello\n\nwide world"};
            auto segments = std::vector<nxt::utf8::text_segment>{};
            for (auto segment : nxt::utf8::segments(text))
                segments.push_back(segment);

            using kind = nxt::utf8::text_segment::kind_t;
            expect(segments.size() == std::size_t{5});
            expect(segments[0].kind == kind::word);
            expect(segments[0].text == "hello");
            expect(segments[1].kind == kind::line_break);
            expect(segments[2].kind == kind::line_break);
            expect(segments[3].text == "wide");
            expect(segments[4].text == "world");
        };

        "split paragraphs on blank lines"_test = [] {
            auto text = std::string{"hello\nwide world\n\nnext"};
            auto paragraphs = std::vector<std::string_view>{};
            for (auto paragraph : nxt::utf8::paragraphs(text))
                paragraphs.push_back(paragraph.text);

            expect(
                paragraphs
                == std::vector<std::string_view>{
                    "hello\nwide world", "next"});
        };

        "treat whitespace-only lines as paragraph breaks"_test = [] {
            auto text = std::string{"\n  \nfirst\n\t \nsecond\n"};
            auto paragraphs = std::vector<std::string_view>{};
            for (auto paragraph : nxt::utf8::paragraphs(text))
                paragraphs.push_back(paragraph.text);

            expect(
                paragraphs
                == std::vector<std::string_view>{"first", "second"});
        };
    }};

// ============================================================================
// text_field rendering
// ============================================================================

static suite text_field_render{
    "Text field rendering", [] {
        using nxt::tui::TextField;
        using nxt::tui::text_field;
        using nxt::utf8::byte_offset;

        "renders text with cursor at end"_test = [] {
            auto out =
                render_row(text_field(TextField{"hi", byte_offset(2)}), 10);
            expect(out.text.starts_with("hi"));
            expect(out.cursor_cell == std::optional<std::size_t>{2});
        };

        "renders cursor in middle of text"_test = [] {
            auto out = render_row(
                text_field(TextField{"hello", byte_offset(2)}), 10);
            expect(out.text.starts_with("hello"));
            expect(out.cursor_cell == std::optional<std::size_t>{2});
        };

        "renders placeholder when empty"_test = [] {
            auto out = render_row(
                text_field(TextField{}, {.placeholder = "type here"}), 20);
            expect(out.text.starts_with("type here"));
        };

        "renders prefix"_test = [] {
            auto out = render_row(
                text_field(
                    TextField{"q", byte_offset(1)}, {.prefix = "> "}),
                10);
            expect(out.text.starts_with("> q"));
            expect(
                out.cursor_cell
                == std::optional<std::size_t>{3}); // after prefix+text
        };

        "scrolls to keep mid-text cursor visible"_test = [] {
            // 10-char text, cursor at byte 7, 5-wide field.  Right-edge
            // tracking puts cells 3..7 in view ("34567"); cursor at col 4.
            auto out = render_row(
                text_field(TextField{"0123456789", byte_offset(7)}), 5);
            expect(out.text == "34567") << "got '" << out.text << "'";
            expect(out.cursor_cell == std::optional<std::size_t>{4});
        };

        "cursor past end leaves blank cell at right edge"_test = [] {
            // Cursor sits one past the last char.  4 chars visible before
            // it; the cursor cell itself is blank (space) under reverse
            // video.
            auto out = render_row(
                text_field(TextField{"0123456789", byte_offset(10)}), 5);
            expect(out.text == "6789 ") << "got '" << out.text << "'";
            expect(out.cursor_cell == std::optional<std::size_t>{4});
        };

        "hide the cursor when unfocused"_test = [] {
            auto out = render_row(
                text_field(
                    TextField{"hi", byte_offset(1)}, {.focused = false}),
                10);
            expect(!out.cursor_cell.has_value());
        };
    }};

// ============================================================================
// apply_key
// ============================================================================

static suite text_field_apply_key{
    "Text field key handling", [] {
        using nxt::tui::TextField;
        using nxt::tui::apply_key;
        using nxt::input::Key;
        using nxt::input::KeyEvent;
        using nxt::utf8::byte_offset;

        "insert text events"_test = [] {
            TextField f;
            auto e = KeyEvent{};
            e.key = Key::character;
            e.text = "x";
            expect(apply_key(f, e));
            expect(f.text == "x");
        };

        "erase on backspace"_test = [] {
            TextField f{"ab", byte_offset(2)};
            expect(apply_key(f, [&] {
                KeyEvent e;
                e.key = Key::backspace;
                return e;
            }()));
            expect(f.text == "a");
        };

        "move the cursor on left arrow"_test = [] {
            TextField f{"ab", byte_offset(2)};
            expect(apply_key(f, [&] {
                KeyEvent e;
                e.key = Key::left;
                return e;
            }()));
            expect(f.cursor_byte == byte_offset(1));
        };

        "ignore release events"_test = [] {
            TextField f{"a", byte_offset(1)};
            KeyEvent e;
            e.key = Key::backspace;
            e.type = nxt::input::EventType::release;
            expect(!apply_key(f, e));
            expect(f.text == "a");
        };

        "return false for unknown keys"_test = [] {
            TextField f;
            KeyEvent e;
            e.key = Key::f5;
            expect(!apply_key(f, e));
        };
    }};

} // namespace nxt::test
