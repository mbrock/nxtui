#include "nxt/baltics.hpp"
#include "nxt/png.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

// Demo: build a small layout that exercises styling, then write it to
// a PNG so we can visually sanity-check the screenshot path.

int main(int argc, char ** argv)
{
    using namespace nxt;
    using namespace nxt::tui;

    std::string out = "nxtshot.png";
    if (argc > 1) {
        if (std::string_view{argv[1]} == "--help"
            || std::string_view{argv[1]} == "-h") {
            std::puts("usage: nxtshot [output.png]");
            return EXIT_SUCCESS;
        }
        out = argv[1];
    }

    const auto p = theme::baltic_church;

    auto title = fg(Rgba8{p.cyan, 255}) | bold;
    auto muted = fg(Rgba8{p.fg_muted, 255});
    auto good  = fg(Rgba8{p.green, 255}) | bold;
    auto warn  = fg(Rgba8{p.amber, 255}) | bold;
    auto bad   = fg(Rgba8{p.coral, 255}) | bold;

    auto layout = column(
        row(
            text("nxtshot ", title),
            text("ready ", good),
            text("model ", muted),
            text("baltic-church"),
            text("  turns ", muted),
            text("3")),
        hrule(),
        row(text("ok    ", good),  text("connection established")),
        row(text("warn  ", warn),  text("retrying in "), text("250ms", warn)),
        row(text("err   ", bad),   text("token rate-limited")),
        hrule(),
        row(text("> ", title), text("type a question, press enter to send")));

    Size cells{60 * ch, 8 * ln};
    nxt::png::write_layout(layout, cells, out);
    std::printf("wrote %s\n", out.c_str());
    return EXIT_SUCCESS;
}
