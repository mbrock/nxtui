#include <nxt/tui_terminal.hpp>
#include <nxtio/subprocess.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>

namespace nxt::test {

using namespace boost::ut;

std::string trimmed_row(const nxt::vterm::Terminal & term, int row)
{
    auto text = term.get_row_text(row);
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

suite subprocess_tests = [] {
    "captures pty output into vterm asynchronously"_test = [] {
        auto scheduler = nxt::io_scheduler::make_unique();
        auto session = nxt::subprocess::PtySession::spawn({
            .argv = {
                "/bin/sh",
                "-lc",
                "printf 'hello\\n'; printf '\\033[31mred\\033[0m\\n'"},
            .cwd = {},
            .environment = {},
            .environment_overlay = {},
            .size = {20 * ch, 4 * ln},
        });

        int damage_count = 0;
        auto status = nxt::sync_wait(session.read_loop(
            *scheduler,
            {},
            [&damage_count] { ++damage_count; }));

        expect(status.success());
        expect(damage_count > 0_i);
        expect(trimmed_row(session.terminal(), 0) == "hello");
        expect(trimmed_row(session.terminal(), 1) == "red");

        auto cell = session.terminal().get_cell(1, 0);
        expect(cell.has_value());
        expect(cell->fg.is_indexed() && cell->fg.c.indexed.idx == 1);
    };

    "writes input through the pty"_test = [] {
        auto scheduler = nxt::io_scheduler::make_unique();
        auto session = nxt::subprocess::PtySession::spawn({
            .argv = {
                "/bin/sh",
                "-lc",
                "IFS= read -r line; printf '<%s>\\n' \"$line\""},
            .cwd = {},
            .environment = {},
            .environment_overlay = {},
            .size = {30 * ch, 4 * ln},
        });

        nxt::sync_wait(session.write_all(*scheduler, "roundtrip\n"));
        auto status = nxt::sync_wait(session.read_loop(*scheduler));

        expect(status.success());
        auto screen = session.terminal().get_screen_text();
        expect(screen.find("<roundtrip>") != std::string::npos);
    };

    "resizes vterm and pty together"_test = [] {
        auto session = nxt::subprocess::PtySession::spawn({
            .argv = {"/bin/sh", "-lc", "stty size; exit"},
            .cwd = {},
            .environment = {},
            .environment_overlay = {},
            .size = {17 * ch, 5 * ln},
        });
        session.resize({23 * ch, 7 * ln});

        auto [rows, cols] = session.terminal().get_size();
        expect(rows == 7_i);
        expect(cols == 23_i);

        auto scheduler = nxt::io_scheduler::make_unique();
        auto status = nxt::sync_wait(session.read_loop(*scheduler));
        expect(status.success());
        expect(session.terminal().get_screen_text().find("7 23")
               != std::string::npos);
    };

    "overlays child environment"_test = [] {
        auto scheduler = nxt::io_scheduler::make_unique();
        auto session = nxt::subprocess::PtySession::spawn({
            .argv = {
                "/bin/sh",
                "-lc",
                "printf '<%s><%s>\\n' \"$TERM\" \"$COLORTERM\""},
            .cwd = {},
            .environment = {"TERM=dumb", "COLORTERM=none"},
            .environment_overlay = {
                "TERM=xterm-256color",
                "COLORTERM=truecolor"},
            .size = {40 * ch, 4 * ln},
        });

        auto status = nxt::sync_wait(session.read_loop(*scheduler));

        expect(status.success());
        expect(session.terminal().get_screen_text().find(
                   "<xterm-256color><truecolor>")
               != std::string::npos);
    };

    "encodes structured keys as shell pty bytes"_test = [] {
        auto session = nxt::subprocess::PtySession::spawn({
            .argv = {"/bin/sh", "-lc", "exit"},
            .cwd = {},
            .environment = {},
            .environment_overlay = {},
            .size = {20 * ch, 4 * ln},
        });

        auto ctrl_a = nxt::input::KeyEvent{};
        ctrl_a.key = nxt::input::Key::character;
        ctrl_a.mods.ctrl = true;
        ctrl_a.codepoint = 'a';
        ctrl_a.text = "a";
        ctrl_a.raw = "\x1b[97;5u";
        expect(session.encode_key(ctrl_a) == std::string{"\x01", 1});

        nxt::input::KeyEvent ctrl_e = ctrl_a;
        ctrl_e.codepoint = 'e';
        ctrl_e.text = "e";
        ctrl_e.raw = "\x1b[101;5u";
        expect(session.encode_key(ctrl_e) == std::string{"\x05", 1});

        nxt::input::KeyEvent ctrl_e_with_base = ctrl_e;
        ctrl_e_with_base.base_layout_codepoint = 'd';
        ctrl_e_with_base.raw = "\x1b[101::100;5u";
        expect(session.encode_key(ctrl_e_with_base)
               == std::string{"\x05", 1});

        nxt::input::KeyEvent ctrl_k_with_base = ctrl_a;
        ctrl_k_with_base.codepoint = 'k';
        ctrl_k_with_base.base_layout_codepoint = 'v';
        ctrl_k_with_base.text = "k";
        ctrl_k_with_base.raw = "\x1b[107::118;5u";
        expect(session.encode_key(ctrl_k_with_base)
               == std::string{"\x0b", 1});

        auto associated_text = nxt::input::KeyEvent{};
        associated_text.key = nxt::input::Key::character;
        associated_text.text = "\xc3\xa5";
        associated_text.raw = "\x1b[0;1;229u";
        expect(session.encode_key(associated_text) == "\xc3\xa5");

        auto super_chord = nxt::input::KeyEvent{};
        super_chord.key = nxt::input::Key::character;
        super_chord.mods.super = true;
        super_chord.codepoint = 'x';
        super_chord.text = "x";
        super_chord.raw = "\x1b[120;9u";
        expect(session.encode_key(super_chord).empty());

        auto unknown = nxt::input::KeyEvent{};
        unknown.key = nxt::input::Key::unknown;
        unknown.raw = "\x1b[999;9u";
        expect(session.encode_key(unknown).empty());

        auto ctrl_shift_i = nxt::input::KeyEvent{};
        ctrl_shift_i.key = nxt::input::Key::character;
        ctrl_shift_i.mods.shift = true;
        ctrl_shift_i.mods.ctrl = true;
        ctrl_shift_i.codepoint = 'i';
        ctrl_shift_i.text = "I";
        ctrl_shift_i.raw = "\x1b[105;6;73u";
        expect(session.encode_key(ctrl_shift_i).empty());
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
