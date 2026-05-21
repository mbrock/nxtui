#include <nxtio/input.hpp>

#include <boost/ut.hpp>

#include <cstdint>

namespace nxt::test {

using namespace boost::ut;
using nxt::input::EventType;
using nxt::input::Key;

static suite input_parser_tests{
    "Input parser", [] {
        "text"_test = [] {
            "parses plain UTF-8"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("a\xc4\x89");

                expect(events.size() == 2_ul);
                expect(events[0].key == Key::character);
                expect(events[0].text == "a");
                expect(events[0].codepoint == std::uint32_t{97});
                expect(events[1].key == Key::character);
                expect(events[1].text == "\xc4\x89");
                expect(events[1].codepoint == std::uint32_t{265});
            };

            "waits for partial UTF-8"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\xc4");
                expect(events.empty());

                events = parser.feed("\x89");
                expect(events.size() == 1_ul);
                expect(events[0].key == Key::character);
                expect(events[0].text == "\xc4\x89");
                expect(events[0].codepoint == std::uint32_t{265});
            };

            "advances past invalid bytes"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\xffx");

                expect(events.size() == 2_ul);
                expect(events[0].key == Key::unknown);
                expect(events[1].key == Key::character);
                expect(events[1].text == "x");
            };
        };

        "controls"_test = [] {
            "parses simple keys"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\r\t\x7f");

                expect(events.size() == 3_ul);
                expect(events[0].key == Key::enter);
                expect(events[1].key == Key::tab);
                expect(events[2].key == Key::backspace);
            };

            "falls back for legacy Ctrl letters"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x01");

                expect(events.size() == 1_ul);
                expect(events[0].key == Key::character);
                expect(events[0].mods.ctrl);
                expect(events[0].text == "a");
            };
        };

        "Kitty CSI u"_test = [] {
            "parses modified characters"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[97;5u");

                expect(events.size() == 1_ul);
                expect(events[0].key == Key::character);
                expect(events[0].codepoint == std::uint32_t{97});
                expect(events[0].text == "a");
                expect(events[0].mods.ctrl);
                expect(!events[0].mods.alt);
            };

            "parses event type and associated text"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[97;1;97u\x1b[97;1:3;97u");

                expect(events.size() == 2_ul);
                expect(events[0].key == Key::character);
                expect(events[0].type == EventType::press);
                expect(events[0].codepoint == std::uint32_t{97});
                expect(events[0].text == "a");
                expect(events[0].is_text());

                expect(events[1].key == Key::character);
                expect(events[1].type == EventType::release);
                expect(events[1].text == "a");
                expect(!events[1].is_text());
            };

            "parses repeat text events"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[97;1:2;97u");

                expect(events.size() == 1_ul);
                expect(events[0].type == EventType::repeat);
                expect(events[0].is_text());
            };

            "parses alternate keys"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[61:43;6;43u");

                expect(events.size() == 1_ul);
                expect(events[0].key == Key::character);
                expect(events[0].codepoint == std::uint32_t{61});
                expect(events[0].shifted_codepoint.has_value());
                expect(*events[0].shifted_codepoint == std::uint32_t{43});
                expect(events[0].mods.shift);
                expect(events[0].mods.ctrl);
                expect(events[0].text == "+");
            };

            "parses associated text without a key code"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[0;1;229u");

                expect(events.size() == 1_ul);
                expect(events[0].key == Key::character);
                expect(events[0].codepoint == std::uint32_t{0});
                expect(events[0].text == "\xc3\xa5");
                expect(events[0].is_text());
            };

            "recognizes Ctrl-C"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[99;5u");

                expect(events.size() == 1_ul);
                expect(events[0].is_ctrl_c());
                expect(!events[0].is_text());
            };

            "parses Escape"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[27u");

                expect(events.size() == 1_ul);
                expect(events[0].key == Key::escape);
            };

            "parses arrows and modifiers"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[1;6D\x1b[1C");

                expect(events.size() == 2_ul);
                expect(events[0].key == Key::left);
                expect(events[0].mods.shift);
                expect(events[0].mods.ctrl);
                expect(events[1].key == Key::right);
            };

            "parses tilde navigation keys"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[3~\x1b[5;3~");

                expect(events.size() == 2_ul);
                expect(events[0].key == Key::delete_);
                expect(events[1].key == Key::page_up);
                expect(events[1].mods.alt);
            };

            "waits for partial escape sequences"_test = [] {
                nxt::input::Parser parser;
                auto events = parser.feed("\x1b[1;");
                expect(events.empty());

                events = parser.feed("5A");
                expect(events.size() == 1_ul);
                expect(events[0].key == Key::up);
                expect(events[0].mods.ctrl);
            };
        };
    }};

} // namespace nxt::test
