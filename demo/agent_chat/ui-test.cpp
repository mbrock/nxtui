#include "view.hpp"
#include <nxtui/sdl.hpp>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace nxtui::ui;
using nxtui::Emphasis;
using nxtui::one;
using nxtui::Rgba8;

void expect(bool ok, const char * message)
{
    if (!ok)
        throw std::runtime_error{message};
}

void check(bool ok)
{
    if (!ok)
        throw std::runtime_error{SDL_GetError()};
}

// Deliberately fractional, independently specified text advances. This
// tests layout's contract with a shaper, not codepoint/cell counting.
struct Metrics : TextMetrics
{
    std::shared_ptr<const ShapedText>
    shape(std::string_view, Width width, Emphasis) override
    {
        auto result = std::make_shared<ShapedText>();
        if (width > Width{})
            result->size = {
                std::min(7.5 * ch, width),
                std::ceil(7.5 / width.count()) * ln};
        return result;
    }
};

void layout_tests()
{
    Metrics metrics;
    auto first = Space{{{0.5 * ch, 1.0 * one}, HeightHint::fixed(1 * ln)}};
    auto second =
        Space{{{1.25 * ch, 3.0 * one}, HeightHint::fixed(1 * ln)}};
    auto layout =
        row(0.25 * ch,
            surface(bg(Rgba8::red()), first),
            surface(bg(Rgba8::blue()), second));
    auto frame = paint(layout, metrics, {6.5 * ch, 2 * ln});
    expect(frame.operations.size() == 2, "two fractional children paint");
    auto a = std::get<FillRect>(frame.operations[0]);
    auto b = std::get<FillRect>(frame.operations[1]);
    expect(
        a.rect.size.w == 1.625 * ch,
        "1/4 of positive leftover, no integer truncation");
    expect(
        b.rect.origin.x == 1.875 * ch && b.rect.size.w == 4.625 * ch,
        "3/4 leftover plus fractional minimum and gap");
    expect(
        b.rect.origin.x + b.rect.size.w == 6.5 * ch,
        "last edge reaches available width");

    auto vertical = column(
        0.25 * ln,
        surface(bg(Rgba8::red()), grow_height(space(), 1)),
        surface(bg(Rgba8::blue()), grow_height(space(), 3)));
    auto vertical_frame = paint(vertical, metrics, {30 * ch, 6.5 * ln});
    auto lower = std::get<FillRect>(vertical_frame.operations[1]);
    expect(
        lower.rect.origin.y == 1.8125 * ln
            && lower.rect.size.h == 4.6875 * ln,
        "column uses line extent and fractional weighted leftover");

    auto reflow =
        row(0.5 * ch, grow_width(text("shaped")), space(2 * ch, 1 * ln));
    Context measuring{
        metrics, frame, {{}, frame.size}, {{}, frame.size}, {}};
    expect(
        reflow.measure(measuring, 6 * ch).height.min == 3 * ln,
        "row cross-axis height measures text at its assigned flex width");

    frame = paint(layout, metrics, {1 * ch, 2 * ln});
    b = std::get<FillRect>(frame.operations[1]);
    expect(
        b.rect.size.w == 1.25 * ch && b.clip.size.w == 0.25 * ch,
        "shortage clips requested geometry, not negative flex");
    expect(
        b.clip.contains({0.99 * ch, 1 * ln})
            && !b.clip.contains({1 * ch, 1 * ln}),
        "hit edges are half open");

    frame = Frame{{2 * ch, 2 * ln}, {}};
    Context context{metrics, frame, {{}, frame.size}, {{}, frame.size}, {}};
    auto child = context.child({{-0.25 * ch, 0.5 * ln}, {3 * ch, 3 * ln}});
    child.stroke(Rgba8::red(), 0.125 * ch);
    auto stroke = std::get<StrokeRect>(frame.operations.front());
    expect(
        stroke.rect.origin.x == -0.25 * ch && stroke.rect.size.w == 3 * ch,
        "clipping preserves outline geometry");
    expect(
        stroke.clip == Rect{{0 * ch, 0.5 * ln}, {2 * ch, 1.5 * ln}},
        "fractional intersection across both axes");

    auto wrapped = fixed_width(
        4 * ch,
        padding(Insets::symmetric(0.25 * ch, 0.5 * ln), text("shaped")));
    auto measured = wrapped.measure(context, 40 * ch);
    expect(
        measured.width.min == 4 * ch && measured.height.min == 4 * ln,
        "fixed width constrains wrapping before measuring height");
    frame = paint(wrapped, metrics, {4 * ch, 4 * ln});
    expect(
        std::get<GlyphRun>(frame.operations.front()).text->size.h == 3 * ln,
        "measuring and painting use the same wrap width");

    frame = paint(
        text("reverse", reverse | fg(Rgba8::red()) | bg(Rgba8::blue())),
        metrics,
        {10 * ch, 2 * ln});
    expect(
        std::get<FillRect>(frame.operations[0]).color == Rgba8::red()
            && std::get<GlyphRun>(frame.operations[1]).color
                   == Rgba8::blue(),
        "shared reverse style swaps foreground and background");
    std::cout
        << "Fractional allocation, shortage, hit edges, clipping, constrained wrapping and shared styles passed\n";
}

void native_tests(const char * font_file)
{
    check(SDL_Init(0));
    check(TTF_Init());
    auto target =
        std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>{
            SDL_CreateSurface(160, 100, SDL_PIXELFORMAT_RGBA32),
            SDL_DestroySurface};
    check(target != nullptr);
    auto renderer =
        std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>{
            SDL_CreateSoftwareRenderer(target.get()), SDL_DestroyRenderer};
    check(renderer != nullptr);
    auto font = std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)>{
        TTF_OpenFont(font_file, 20), TTF_CloseFont};
    check(font != nullptr);
    int cp = 0, unused = 0;
    check(TTF_GetStringSize(font.get(), "0", 1, &cp, &unused));
    auto lp = TTF_GetFontLineSkip(font.get());
    SdlPainter painter{renderer.get(), font_file};
    expect(
        painter.shape("WWW", 100 * ch, Emphasis::none)->size.w
            > painter.shape("iii", 100 * ch, Emphasis::none)->size.w * 2,
        "proportional shaped advances, not cell counts");
    expect(
        painter.shape("é", 100 * ch, Emphasis::none)->size.w
            == painter.shape("é", 100 * ch, Emphasis::none)->size.w,
        "HarfBuzz composed/decomposed accents");
    expect(
        painter.shape("one two three four", 6 * ch, Emphasis::none)->size.h
            > painter.shape("one two three four", 60 * ch, Emphasis::none)
                  ->size.h,
        "native wrapping changes measured height");

    check(SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255));
    check(SDL_RenderClear(renderer.get()));
    SDL_Rect caller_clip{8, 4, 100, 80};
    check(SDL_SetRenderClipRect(renderer.get(), &caller_clip));
    auto size = painter.viewport();
    auto clip = Rect{{0.5 * ch, 0.25 * ln}, {1.25 * ch, 0.5 * ln}};
    Frame frame{size, {FillRect{{{}, size}, Rgba8(255, 255, 255), clip}}};
    painter.draw(frame);
    SDL_Rect restored{};
    check(SDL_GetRenderClipRect(renderer.get(), &restored));
    expect(
        SDL_RenderClipEnabled(renderer.get()) && restored.x == 8
            && restored.y == 4 && restored.w == 100 && restored.h == 80,
        "caller renderer clip restored");
    check(SDL_SetRenderClipRect(renderer.get(), nullptr));
    auto capture =
        std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>{
            SDL_RenderReadPixels(renderer.get(), nullptr),
            SDL_DestroySurface};
    check(capture != nullptr);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 160; ++x) {
            Uint8 r, g, b, a;
            check(
                SDL_ReadSurfacePixel(capture.get(), x, y, &r, &g, &b, &a));
            auto inside = x >= std::max(8L, std::lround(0.5 * cp))
                          && x < std::lround(1.75 * cp)
                          && y >= std::max(4L, std::lround(0.25 * lp))
                          && y < std::lround(0.75 * lp);
            expect(
                r == (inside ? 255 : 0) && g == r && b == r,
                "fractional clips round endpoints only at the pixel boundary");
        }

    nxtui::chat::State state;
    state.messages = {
        {false, "A message that wraps in a narrow viewport."},
        {true, "WWW iii café é"}};
    state.status = nxtui::chat::Status::streaming;
    nxtui::chat::Hits hits;
    auto chat_frame =
        paint(nxtui::chat::view(state, hits), painter, {40 * ch, 28 * ln});
    expect(
        !hits.composer.empty() && !hits.action.empty(),
        "visible chat input/action hit regions");
    expect(
        hits.composer.origin.x + hits.composer.size.w
            < hits.action.origin.x,
        "composer and action do not overlap");
    for (auto & op : chat_frame.operations)
        std::visit(
            [](auto & operation) {
                expect(
                    operation.clip.origin.x >= 0 * ch
                        && operation.clip.origin.y >= 0 * ln
                        && operation.clip.origin.x + operation.clip.size.w
                               <= 40 * ch
                        && operation.clip.origin.y + operation.clip.size.h
                               <= 28 * ln,
                    "chat paint stays clipped to viewport");
            },
            op);
    std::cout
        << "SDL3_ttf shaping/wrapping, per-pixel fractional clipping, clip restoration and chat hit regions passed\n";
}
} // namespace

int main(int argc, char ** argv)
{
    try {
        layout_tests();
        if (argc == 2)
            native_tests(argv[1]);
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    TTF_Quit();
    SDL_Quit();
}
