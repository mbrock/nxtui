#pragma once

#include <nxtui/ui.hpp>

namespace nxtui::chat {

using ui::ch;
using ui::ln;

enum class Status { ready, streaming, stopped, error };

struct Message
{
    bool assistant;
    std::string text;
};

/// Transport-independent view state. Publish/edit this on the UI thread:
/// append deltas to messages.back().text and change status/detail on
/// finish.
struct State
{
    std::vector<Message> messages;
    std::string composer;
    std::string preedit;
    Status status = Status::ready;
    std::string detail = "Fixture data · no network connection";
    ui::Height scroll_from_bottom{};
};

struct Hits
{
    ui::Rect composer, action;
};

namespace detail {
using namespace ui;
inline constexpr Rgba8 background{16, 21, 31};
inline constexpr Rgba8 panel{24, 34, 48};
inline constexpr Rgba8 muted{143, 162, 184};
inline constexpr Rgba8 accent{107, 219, 186};

template<Layout L>
struct Hit
{
    L child;
    Rect * target;

    Measure measure(const Context & context, Width width) const
    {
        return child.measure(context, width);
    }

    void paint(const Context & context) const
    {
        *target = intersect(context.bounds, context.clip);
        child.paint(context);
    }
};

inline auto message_card(const Message & message)
{
    return surface(
        bg(message.assistant ? panel : Rgba8(28, 43, 45)),
        padding(
            Insets::symmetric(1.5 * ch, 0.75 * ln),
            column(
                0.25 * ln,
                text(
                    message.assistant ? "NXT" : "YOU",
                    bold | fg(message.assistant ? accent : muted),
                    false),
                text(
                    message.text.empty() ? "Waiting for text…"
                                         : message.text))));
}

struct Transcript
{
    const State * state;

    Measure measure(const Context &, Width) const
    {
        return {WidthHint::grow(), HeightHint::grow()};
    }

    void paint(const Context & context) const
    {
        using Card = decltype(message_card(Message{}));
        auto cards = std::vector<Card>{};
        for (const auto & message : state->messages)
            cards.push_back(message_card(message));
        auto content = column(0.75 * ln, std::move(cards));
        auto height =
            content.measure(context, context.bounds.size.w).height.min;
        auto maximum = std::max(Height{}, height - context.bounds.size.h);
        auto offset =
            maximum
            - std::clamp(state->scroll_from_bottom, Height{}, maximum);
        content.paint(context.child(
            {{{}, -offset}, {context.bounds.size.w, height}}));
    }
};
} // namespace detail

/// A small custom view built from the same public value compositions as
/// every other layout. It owns no stream, credentials, timers, or renderer.
struct View
{
    const State * state;
    Hits * hits;

    ui::Measure measure(const ui::Context &, ui::Width) const
    {
        return {ui::WidthHint::grow(), ui::HeightHint::grow()};
    }

    void paint(const ui::Context & context) const
    {
        using namespace detail;
        auto size = context.bounds.size;
        auto width = std::max(Width{}, std::min(76 * ch, size.w - 3 * ch));
        auto side = std::max(Width{}, (size.w - width) / 2);
        auto active = state->status == Status::streaming;
        auto status = active                             ? "● Streaming"
                      : state->status == Status::error   ? "● Error"
                      : state->status == Status::stopped ? "● Stopped"
                                                         : "● Ready";
        auto status_color =
            state->status == Status::error ? Rgba8(248, 133, 130) : accent;
        auto header = column(
            0.25 * ln,
            row(0.75 * ch,
                text("NXT", bold | fg(accent), false),
                text("Agent chat", bold, false),
                stretch(),
                text(status, fg(status_color), false)),
            text(
                width < 40 * ch ? "Graphical chat"
                                : "Character rhythm. Graphical paint.",
                fg(muted),
                false));

        auto typed = state->composer.empty() && state->preedit.empty()
                         ? std::string{"Write a message…"}
                         : state->composer + state->preedit + "▏";
        auto input = fixed_height(
            2.75 * ln,
            border(
                Rgba8(67, 89, 111),
                surface(
                    bg(panel),
                    padding(
                        Insets::symmetric(1 * ch, 0.5 * ln),
                        text(
                            std::move(typed),
                            fg(state->composer.empty()
                                   ? muted
                                   : Rgba8(230, 233, 240)))))));
        auto button = fixed_height(
            2.75 * ln,
            surface(
                bg(active ? Rgba8(73, 48, 48) : Rgba8(41, 81, 70)),
                padding(
                    Insets::symmetric(1.25 * ch, 0.75 * ln),
                    text(
                        active ? "Stop" : "Send ↑",
                        bold | fg(active ? Rgba8(255, 177, 167) : accent),
                        false))));
        auto composer = column(
            0.5 * ln,
            text("MESSAGE", fg(muted), false),
            row(0.75 * ch,
                grow_width(Hit{std::move(input), &hits->composer}),
                Hit{std::move(button), &hits->action}));
        auto footer = text(
            state->detail
                + (width < 55 * ch
                       ? "\nEnter sends · Esc stops · Wheel scrolls"
                       : "  ·  Enter sends / Shift+Enter newline  ·  Wheel scrolls"),
            fg(muted));
        auto body = padding(
            {side, side, 0.75 * ln, 0.75 * ln},
            column(
                0.75 * ln,
                std::move(header),
                Transcript{state},
                std::move(composer),
                std::move(footer)));
        surface(bg(background), std::move(body)).paint(context);
    }
};

inline View view(const State & state, Hits & hits)
{
    return {&state, &hits};
}

} // namespace nxtui::chat
