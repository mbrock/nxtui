#include "nxt/baltics.hpp"
#include "nxt/text_field.hpp"
#include "nxt/tui.hpp"
#include "nxt/units.hpp"
#include "nxtio/app.hpp"
#include "nxtio/async.hpp"
#include "nxtio/input.hpp"
#include "nxtio/llm.hpp"
#include "nxtio/net.hpp"
#include "nxtio/text_field.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::chat {

using namespace nxt::tui;

struct message
{
    std::string role;
    std::string text;
};

struct state
{
    TextField input;
    std::vector<message> messages;
    std::string model = "gpt-5.3";
    std::string status = "ready";
    std::string error;
    bool busy = false;
};

std::string fit(std::string text, std::size_t width)
{
    if (text.size() <= width)
        return text;
    if (width <= 3)
        return text.substr(0, width);
    return text.substr(0, width - 3) + "...";
}

std::string prompt_for(const state & s)
{
    std::string prompt =
        "You are a concise terminal chat assistant. Continue the conversation.\n";
    for (const auto & msg : s.messages) {
        prompt += msg.role;
        prompt += ": ";
        prompt += msg.text;
        prompt += "\n";
    }
    prompt += "assistant:";
    return prompt;
}

auto input_field(const state & s)
{
    const auto p = theme::baltic_church;
    return text_field(
        s.input,
        {
            .prefix      = "> ",
            .placeholder = s.busy ? "" : "ask anything (esc to quit)",
            .style = {
                .fg = s.busy ? Rgba8{p.fg_muted, 255}
                             : Rgba8{p.fg, 255},
                .bg             = Rgba8{p.bg_elev, 255},
                .prefix_fg      = s.busy ? Rgba8{p.fg_muted, 255}
                                         : Rgba8{p.cyan, 255},
                .placeholder_fg = Rgba8{p.fg_subtle, 255},
            },
            .focused = !s.busy,
        });
}

std::string text_delta(
    const nxt::io::llm::stream_event & event,
    std::string_view type)
{
    if (event.type != type)
        return {};
    return event.payload.value("delta", std::string{});
}

auto chat_hud(const state & s)
{
    const auto p = theme::baltic_church;
    auto accent = fg(Rgba8{p.cyan, 255}) | bold;
    auto muted  = fg(Rgba8{p.fg_muted, 255});
    auto good   = fg(Rgba8{p.green, 255}) | bold;
    auto bad    = fg(Rgba8{p.coral, 255}) | bold;
    auto status_style = s.error.empty()
        ? (s.busy ? muted : good)
        : bad;
    auto turns = std::to_string(s.messages.size() / 2);

    return column(
        row(
            text("nxtchat ", accent),
            text(s.busy ? "streaming " : "ready ", status_style),
            text("model ", muted),
            text(fit(s.model, 24)),
            text(" turns " + turns, muted),
            text(s.error.empty() ? "" : "  " + fit(s.error, 64), bad)),
        input_field(s));
}

nxt::task<> ask_model(nxt::ui::UIRuntime & runtime, state & s, std::string api_key)
{
    auto reply = std::string{};

    try {
        s.busy = true;
        s.status = "connecting";
        s.error.clear();
        runtime.signal_damage();

        auto transport = co_await nxt::io::net::connect_tls(
            runtime.scheduler_handle(),
            nxt::io::net::endpoint{
                .host = "api.openai.com",
                .port = 443,
            });

        s.status = "streaming";
        runtime.signal_damage();

        auto request = nxt::io::llm::openai_responses_request{
            .api_key = std::move(api_key),
            .model = s.model,
            .input = prompt_for(s),
            .max_output_tokens = 1200,
            .reasoning_effort = "low",
            .reasoning_summary = {},
        };

        using transport_t = decltype(transport);
        auto stream = nxt::io::llm::openai_response_stream<transport_t>{
            transport, runtime.get_stop_token()};
        co_await stream.connect(request);

        while (auto event = co_await stream.next()) {
            auto delta = text_delta(*event, "response.output_text.delta");
            if (!delta.empty()) {
                reply += delta;
                runtime.print(delta);
            }
        }
        co_await transport.shutdown();

        if (!reply.empty() && reply.back() != '\n')
            runtime.print("\n");
        s.messages.push_back(message{.role = "assistant", .text = reply});
        s.status = "ready";
    } catch (const std::exception & e) {
        if (!runtime.shutdown_requested()) {
            s.error = e.what();
            s.status = "error";
            runtime.println(std::string{"error: "} + e.what());
        }
    }

    s.busy = false;
    runtime.signal_damage();
}

nxt::task<> update(nxt::ui::UIRuntime & runtime, state & s)
{
    runtime.println("nxtchat: type a question, press Enter to send, Esc to quit.");

    while (!runtime.shutdown_requested()) {
        auto event = co_await runtime.next_input();
        if (!event)
            co_return;

        if (event->type == nxt::input::EventType::release)
            continue;

        if (event->key == nxt::input::Key::escape) {
            runtime.request_shutdown();
            co_return;
        }

        if (s.busy)
            continue;

        if (event->key == nxt::input::Key::enter) {
            if (s.input.empty())
                continue;

            auto question = std::move(s.input.text);
            s.input.clear();
            s.error.clear();
            s.messages.push_back(message{.role = "user", .text = question});

            runtime.println("");
            runtime.println("user: " + question);
            runtime.println("assistant:");
            runtime.signal_damage();

            auto api_key = std::getenv("OPENAI_API_KEY");
            if (api_key == nullptr || std::string_view{api_key}.empty()) {
                s.error = "OPENAI_API_KEY is not set";
                runtime.println("error: OPENAI_API_KEY is not set");
                runtime.signal_damage();
                continue;
            }

            co_await ask_model(runtime, s, api_key);
            continue;
        }

        if (apply_key(s.input, *event))
            runtime.signal_damage();
    }
}

std::string default_model(int argc, char ** argv)
{
    if (argc > 1) {
        auto arg = std::string_view{argv[1]};
        if (arg == "--help" || arg == "-h") {
            std::cout << "usage: nxtchat [model]\n";
            std::exit(EXIT_SUCCESS);
        }
        return std::string{arg};
    }

    if (auto model = std::getenv("OPENAI_MODEL");
        model != nullptr && std::string_view{model}.size() > 0)
        return model;

    return "gpt-5.3";
}

int run(int argc, char ** argv)
{
    auto initial = state{};
    initial.model = default_model(argc, argv);
    return nxt::ui::run(
        std::move(initial),
        [](const state & s) { return chat_hud(s); },
        update);
}

} // namespace nxt::chat

int main(int argc, char ** argv)
{
    return nxt::chat::run(argc, argv);
}
