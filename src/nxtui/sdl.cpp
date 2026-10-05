#include "nxtui/sdl.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <cmath>
#include <map>
#include <stdexcept>

namespace nxtui::ui {
namespace {

void check(bool ok)
{
    if (!ok)
        throw std::runtime_error{SDL_GetError()};
}

SDL_Color color(Rgba8 value)
{
    if (value.is_true_color())
        return {value.r(), value.g(), value.b(), value.a()};
    // Standard xterm palette for graphical uses of the shared color type.
    // Terminal sentinels should already have inherited a concrete style.
    if (!value.is_palette())
        throw std::invalid_argument{"graphical color must be concrete"};
    auto index = value.palette_index();
    constexpr std::array<Rgb8, 16> basic{
        {{0, 0, 0},
         {205, 0, 0},
         {0, 205, 0},
         {205, 205, 0},
         {0, 0, 238},
         {205, 0, 205},
         {0, 205, 205},
         {229, 229, 229},
         {127, 127, 127},
         {255, 0, 0},
         {0, 255, 0},
         {255, 255, 0},
         {92, 92, 255},
         {255, 0, 255},
         {0, 255, 255},
         {255, 255, 255}}};
    if (index < 16) {
        auto c = basic[index];
        return {c.r, c.g, c.b, 255};
    }
    if (index >= 232) {
        auto gray = static_cast<Uint8>(8 + (index - 232) * 10);
        return {gray, gray, gray, 255};
    }
    constexpr std::array<Uint8, 6> ramp{0, 95, 135, 175, 215, 255};
    auto cube = index - 16;
    return {ramp[cube / 36], ramp[(cube / 6) % 6], ramp[cube % 6], 255};
}

using Font = std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)>;
using Engine = std::
    unique_ptr<TTF_TextEngine, decltype(&TTF_DestroyRendererTextEngine)>;

struct SdlText final : ShapedText
{
    std::unique_ptr<TTF_Text, decltype(&TTF_DestroyText)> text{
        nullptr, TTF_DestroyText};
    TTF_TextEngine * engine = nullptr;
    Emphasis emphasis{};
};

} // namespace

struct SdlPainter::Impl
{
    SDL_Renderer * renderer;
    std::string font_file;
    float font_size;
    std::map<TTF_FontStyleFlags, Font> fonts;
    Engine engine{nullptr, TTF_DestroyRendererTextEngine};
    double character_pixels = 1;
    double line_pixels = 1;

    Impl(SDL_Renderer * renderer, std::string file, float size)
        : renderer(renderer)
        , font_file(std::move(file))
        , font_size(size)
    {
        engine.reset(TTF_CreateRendererTextEngine(renderer));
        check(engine != nullptr);
        auto * regular = font(Emphasis::none);
        int width = 0, height = 0;
        check(TTF_GetStringSize(regular, "0", 1, &width, &height));
        character_pixels = std::max(1, width);
        line_pixels = std::max(1, TTF_GetFontLineSkip(regular));
    }

    TTF_Font * font(Emphasis emphasis)
    {
        TTF_FontStyleFlags flags = TTF_STYLE_NORMAL;
        if (has_emphasis(emphasis, Emphasis::bold))
            flags |= TTF_STYLE_BOLD;
        if (has_emphasis(emphasis, Emphasis::italic))
            flags |= TTF_STYLE_ITALIC;
        if (has_emphasis(emphasis, Emphasis::underline))
            flags |= TTF_STYLE_UNDERLINE;
        if (has_emphasis(emphasis, Emphasis::strikethrough))
            flags |= TTF_STYLE_STRIKETHROUGH;
        if (auto found = fonts.find(flags); found != fonts.end())
            return found->second.get();
        auto value =
            Font{TTF_OpenFont(font_file.c_str(), font_size), TTF_CloseFont};
        check(value != nullptr);
        TTF_SetFontStyle(value.get(), flags);
        auto * result = value.get();
        fonts.emplace(flags, std::move(value));
        return result;
    }

    SDL_FRect pixels(Rect rect) const
    {
        return {
            static_cast<float>(rect.origin.x.count() * character_pixels),
            static_cast<float>(rect.origin.y.count() * line_pixels),
            static_cast<float>(rect.size.w.count() * character_pixels),
            static_cast<float>(rect.size.h.count() * line_pixels)};
    }
};

SdlPainter::SdlPainter(
    SDL_Renderer * renderer, std::string file, float size)
    : impl_(std::make_unique<Impl>(renderer, std::move(file), size))
{
}

SdlPainter::~SdlPainter() = default;

std::shared_ptr<const ShapedText>
SdlPainter::shape(std::string_view text, Width width, Emphasis emphasis)
{
    auto run = std::make_shared<SdlText>();
    run->engine = impl_->engine.get();
    run->emphasis = emphasis;
    if (width <= Width{})
        return run;
    run->text.reset(TTF_CreateText(
        impl_->engine.get(),
        impl_->font(emphasis),
        text.data(),
        text.size()));
    check(run->text != nullptr);
    auto wrap = std::isfinite(width.count())
                    ? std::max(
                          1,
                          static_cast<int>(std::floor(
                              width.count() * impl_->character_pixels)))
                    : 0;
    check(TTF_SetTextWrapWidth(run->text.get(), wrap));
    int w = 0, h = 0;
    check(TTF_GetTextSize(run->text.get(), &w, &h));
    run->size = {
        Width{w / impl_->character_pixels}, Height{h / impl_->line_pixels}};
    return run;
}

Size SdlPainter::viewport() const
{
    int w = 0, h = 0;
    check(SDL_GetCurrentRenderOutputSize(impl_->renderer, &w, &h));
    return {
        Width{w / impl_->character_pixels}, Height{h / impl_->line_pixels}};
}

Pos SdlPainter::position(float x, float y) const
{
    float px = 0, py = 0;
    check(SDL_RenderCoordinatesFromWindow(impl_->renderer, x, y, &px, &py));
    return {
        Width{px / impl_->character_pixels},
        Height{py / impl_->line_pixels}};
}

void SdlPainter::draw(const Frame & frame)
{
    auto * renderer = impl_->renderer;

    struct ClipScope
    {
        SDL_Renderer * renderer;
        bool enabled;
        SDL_Rect previous;

        ~ClipScope()
        {
            SDL_SetRenderClipRect(renderer, enabled ? &previous : nullptr);
        }
    };

    auto scope = ClipScope{renderer, SDL_RenderClipEnabled(renderer), {}};
    check(SDL_GetRenderClipRect(renderer, &scope.previous));
    check(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));
    for (const auto & operation : frame.operations) {
        std::visit(
            [&](const auto & op) {
                auto c = color(op.color);
                auto box = impl_->pixels(op.clip);
                auto x = static_cast<int>(std::lround(box.x));
                auto y = static_cast<int>(std::lround(box.y));
                auto right = static_cast<int>(std::lround(box.x + box.w));
                auto bottom = static_cast<int>(std::lround(box.y + box.h));
                auto clip = SDL_Rect{
                    x, y, std::max(0, right - x), std::max(0, bottom - y)};
                if (scope.enabled
                    && !SDL_GetRectIntersection(
                        &clip, &scope.previous, &clip))
                    return;
                if (clip.w == 0 || clip.h == 0)
                    return;
                check(SDL_SetRenderClipRect(renderer, &clip));
                check(SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a));
                using Op = std::decay_t<decltype(op)>;
                if constexpr (std::same_as<Op, FillRect>) {
                    auto rect = impl_->pixels(op.rect);
                    check(SDL_RenderFillRect(renderer, &rect));
                } else if constexpr (std::same_as<Op, StrokeRect>) {
                    auto r = impl_->pixels(op.rect);
                    auto t = std::min(
                        {static_cast<float>(
                             op.thickness.count()
                             * impl_->character_pixels),
                         r.w / 2,
                         r.h / 2});
                    std::array<SDL_FRect, 4> edges{
                        {{r.x, r.y, r.w, t},
                         {r.x, r.y + r.h - t, r.w, t},
                         {r.x, r.y + t, t, r.h - 2 * t},
                         {r.x + r.w - t, r.y + t, t, r.h - 2 * t}}};
                    check(SDL_RenderFillRects(
                        renderer, edges.data(), edges.size()));
                } else {
                    auto & run = dynamic_cast<const SdlText &>(*op.text);
                    if (run.engine != impl_->engine.get())
                        throw std::invalid_argument{
                            "text belongs to another SDL painter"};
                    if (!run.text)
                        return;
                    if (has_emphasis(run.emphasis, Emphasis::faint))
                        c.a /= 2;
                    if (has_emphasis(run.emphasis, Emphasis::conceal))
                        c.a = 0;
                    check(TTF_SetTextColor(
                        run.text.get(), c.r, c.g, c.b, c.a));
                    check(TTF_DrawRendererText(
                        run.text.get(),
                        static_cast<float>(
                            op.origin.x.count() * impl_->character_pixels),
                        static_cast<float>(
                            op.origin.y.count() * impl_->line_pixels)));
                }
            },
            operation);
    }
}

} // namespace nxtui::ui
