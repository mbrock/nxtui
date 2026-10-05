#include "view.hpp"
#include <nxtui/sdl.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace nxtui;

void check(bool ok)
{
    if (!ok)
        throw std::runtime_error{SDL_GetError()};
}

constexpr std::string_view reply =
    "Keep the rhythm, not the raster.\n\n"
    "Layout stays in character and line units — halves, quarters, and multiples included. "
    "Rectangles and shaped text go straight to the renderer. No glyph-cell image sits underneath.\n\n"
    "Text is measured by SDL3_ttf and HarfBuzz: WWW and iii have different advances; "
    "café and é keep their accents. Resize the window to see the same message reflow.\n\n"
    "This reply is fixture data. The view is ready for your real agent transport.";

/// Fixture scheduling is separate from the view's plain state. Replace
/// this with transport delta/status updates; keep the SDL/UI thread owner.
struct Fixture
{
    chat::State & state;
    std::size_t cursor = 0;
    Uint64 next = 0;

    void tick(Uint64 now)
    {
        if (state.status != chat::Status::streaming || now < next)
            return;
        for (int i = 0; i < 3 && cursor < reply.size(); ++i) {
            ++cursor;
            while (cursor < reply.size()
                   && (static_cast<unsigned char>(reply[cursor]) & 0xc0)
                          == 0x80)
                ++cursor;
        }
        state.messages.back().text = reply.substr(0, cursor);
        next = now + 35;
        if (cursor == reply.size())
            state.status = chat::Status::ready;
    }

    void stop()
    {
        if (state.status == chat::Status::streaming)
            state.status = chat::Status::stopped;
    }

    void send()
    {
        if (state.status == chat::Status::streaming
            || state.composer.empty())
            return;
        state.messages.push_back(
            {false, std::exchange(state.composer, {})});
        state.messages.push_back({true, {}});
        state.status = chat::Status::streaming;
        state.scroll_from_bottom = {};
        cursor = 0;
        next = 0;
    }
};

void backspace(std::string & text)
{
    if (text.empty())
        return;
    auto start = text.size() - 1;
    while (start > 0
           && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80)
        --start;
    text.resize(start);
}

void event(
    const SDL_Event & event,
    chat::State & state,
    Fixture & fixture,
    const chat::Hits & hits,
    ui::SdlPainter & painter,
    SDL_Window * window,
    bool & running)
{
    if (event.type == SDL_EVENT_QUIT)
        running = false;
    else if (event.type == SDL_EVENT_TEXT_INPUT) {
        state.composer += event.text.text;
        state.preedit.clear();
    } else if (event.type == SDL_EVENT_TEXT_EDITING)
        state.preedit = event.edit.text;
    else if (event.type == SDL_EVENT_MOUSE_WHEEL)
        state.scroll_from_bottom = std::max(
            ui::Height{},
            state.scroll_from_bottom + event.wheel.y * 3 * ui::ln);
    else if (
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
        && event.button.button == SDL_BUTTON_LEFT) {
        auto point = painter.position(event.button.x, event.button.y);
        if (hits.action.contains(point)) {
            if (state.status == chat::Status::streaming)
                fixture.stop();
            else
                fixture.send();
        } else if (hits.composer.contains(point))
            check(SDL_StartTextInput(window));
    } else if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE)
            fixture.stop();
        else if (event.key.key == SDLK_BACKSPACE)
            backspace(state.composer);
        else if (event.key.key == SDLK_RETURN && state.preedit.empty()) {
            if (event.key.mod & SDL_KMOD_SHIFT)
                state.composer += '\n';
            else
                fixture.send();
        } else if (
            event.key.key == SDLK_V
            && (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
            auto clipboard = std::unique_ptr<char, decltype(&SDL_free)>{
                SDL_GetClipboardText(), SDL_free};
            if (clipboard)
                state.composer += clipboard.get();
        }
    }
}

void exercise_events()
{
    auto input = SDL_Event{};
    input.type = SDL_EVENT_TEXT_INPUT;
    input.text.text = "Native café";
    check(SDL_PushEvent(&input));
    auto key = SDL_Event{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_BACKSPACE;
    check(SDL_PushEvent(&key));
    input.text.text = "é";
    check(SDL_PushEvent(&input));
    key.key.key = SDLK_RETURN;
    check(SDL_PushEvent(&key));
    key.key.key = SDLK_ESCAPE;
    check(SDL_PushEvent(&key));
}

int run(int argc, char ** argv)
{
    std::string font =
        std::string{SDL_GetBasePath()} + "../share/nxtui/DejaVuSans.ttf";
    std::string snapshot, state_name = "streaming";
    int width = 1080, height = 760;
    bool exercise = false;
    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--exercise")
            exercise = true;
        else if (arg == "--help") {
            std::cout
                << "nxt-agent-chat-demo --font font.ttf [--snapshot image.bmp] "
                   "[--state streaming|ready|stopped|error] [--width N --height N] [--exercise]\n";
            return 0;
        } else {
            if (++i == argc)
                throw std::invalid_argument{"option requires a value"};
            if (arg == "--font")
                font = argv[i];
            else if (arg == "--snapshot")
                snapshot = argv[i];
            else if (arg == "--state")
                state_name = argv[i];
            else if (arg == "--width")
                width = std::stoi(argv[i]);
            else if (arg == "--height")
                height = std::stoi(argv[i]);
            else
                throw std::invalid_argument{"unknown option"};
        }
    }
    auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>{
        SDL_CreateWindow(
            "NXT · Agent chat",
            width,
            height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY),
        SDL_DestroyWindow};
    check(window != nullptr);
    auto renderer =
        std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>{
            SDL_CreateRenderer(window.get(), nullptr), SDL_DestroyRenderer};
    check(renderer != nullptr);
    auto painter = ui::SdlPainter{renderer.get(), font};
    auto state = chat::State{};
    state.messages = {
        {false,
         "Can we keep NXT’s character rhythm without the terminal raster?"},
        {true, {}}};
    state.status = chat::Status::streaming;
    auto fixture = Fixture{state};
    if (!snapshot.empty() || exercise) {
        if (state_name == "ready" || exercise) {
            state.messages.back().text = reply;
            state.status = chat::Status::ready;
        } else if (state_name == "streaming")
            state.messages.back().text = reply.substr(0, 218);
        else if (state_name == "stopped") {
            state.messages.back().text = reply.substr(0, 275);
            state.status = chat::Status::stopped;
        } else if (state_name == "error") {
            state.messages.back().text =
                "The fixture stream stopped with an example error. Your draft is still editable.";
            state.status = chat::Status::error;
            state.detail = "Fixture error · send a new message to retry";
            state.composer = "Try a shorter explanation.";
        } else
            throw std::invalid_argument{"unknown snapshot state"};
    }
    check(SDL_StartTextInput(window.get()));
    chat::Hits hits;
    bool running = true;
    if (exercise) {
        check(SDL_SetWindowSize(window.get(), 540, 640));
        check(SDL_SyncWindow(window.get()));
        exercise_events();
    }
    while (running) {
        SDL_Event current{};
        while (SDL_PollEvent(&current))
            event(
                current,
                state,
                fixture,
                hits,
                painter,
                window.get(),
                running);
        if (snapshot.empty() && !exercise)
            fixture.tick(SDL_GetTicks());
        auto frame =
            ui::paint(chat::view(state, hits), painter, painter.viewport());
        painter.draw(frame);
        if (!snapshot.empty()) {
            auto surface =
                std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>{
                    SDL_RenderReadPixels(renderer.get(), nullptr),
                    SDL_DestroySurface};
            check(surface != nullptr);
            check(SDL_SaveBMP(surface.get(), snapshot.c_str()));
            std::cout << "Rendered " << surface->w << "×" << surface->h
                      << " " << state_name << " state to " << snapshot
                      << '\n';
        }
        check(SDL_RenderPresent(renderer.get()));
        if (exercise) {
            if (state.messages.size() != 4
                || state.messages[2].text != "Native café"
                || !state.composer.empty()
                || state.status != chat::Status::stopped)
                throw std::runtime_error{
                    "native input/send/stop exercise failed"};
            int resized_width = 0, resized_height = 0;
            check(SDL_GetWindowSize(
                window.get(), &resized_width, &resized_height));
            if (resized_width != 540 || resized_height != 640)
                throw std::runtime_error{"native resize exercise failed"};
            state.status = chat::Status::streaming;
            for (Uint64 now = 0;
                 state.status == chat::Status::streaming && now < 10000;
                 now += 35)
                fixture.tick(now);
            if (state.status != chat::Status::ready
                || state.messages.back().text != reply)
                throw std::runtime_error{
                    "fixture stream completion failed"};
            std::cout
                << "Native UTF-8 input/backspace/send/stop/resize and fixture stream completion passed\n";
        }
        if (!snapshot.empty() || exercise)
            break;
        SDL_Delay(16);
    }
    return 0;
}
} // namespace

int main(int argc, char ** argv)
{
    int result = 1;
    try {
        check(SDL_Init(SDL_INIT_VIDEO));
        check(TTF_Init());
        result = run(argc, argv);
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
    }
    TTF_Quit();
    SDL_Quit();
    return result;
}
