#include "nxt/tui.hpp"
#include "nxt/units.hpp"
#include "nxtio/app.hpp"
#include "nxtio/async.hpp"
#include "nxtio/input.hpp"
#include "nxtio/llm.hpp"
#include "nxtio/net.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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
    std::string input;
    std::size_t cursor_byte = 0;
    std::vector<message> messages;
    std::string model = "gpt-5.3";
    std::string status = "ready";
    std::string error;
    bool busy = false;
};

std::size_t next_codepoint(std::string_view text, std::size_t byte)
{
    if (byte >= text.size())
        return text.size();
    ++byte;
    while (byte < text.size()
           && (static_cast<unsigned char>(text[byte]) & 0xc0) == 0x80)
        ++byte;
    return byte;
}

std::size_t previous_codepoint(std::string_view text, std::size_t byte)
{
    if (byte == 0)
        return 0;
    --byte;
    while (byte > 0
           && (static_cast<unsigned char>(text[byte]) & 0xc0) == 0x80)
        --byte;
    return byte;
}

std::size_t codepoint_count(std::string_view text)
{
    std::size_t count = 0;
    for (std::size_t byte = 0; byte < text.size();
         byte = next_codepoint(text, byte))
        ++count;
    return count;
}

std::size_t byte_at_cell(std::string_view text, std::size_t cell)
{
    std::size_t byte = 0;
    for (std::size_t i = 0; i < cell && byte < text.size(); ++i)
        byte = next_codepoint(text, byte);
    return byte;
}

std::size_t cell_at_byte(std::string_view text, std::size_t byte)
{
    byte = std::min(byte, text.size());
    std::size_t cell = 0;
    for (std::size_t i = 0; i < byte; i = next_codepoint(text, i))
        ++cell;
    return cell;
}

void apply_edit(state & s, const nxt::input::KeyEvent & event)
{
    using nxt::input::Key;

    s.cursor_byte = std::min(s.cursor_byte, s.input.size());

    if (event.is_text()) {
        s.input.insert(s.cursor_byte, event.text);
        s.cursor_byte += event.text.size();
        return;
    }

    if (event.type == nxt::input::EventType::release)
        return;

    switch (event.key) {
    case Key::backspace:
        if (s.cursor_byte > 0) {
            auto prev = previous_codepoint(s.input, s.cursor_byte);
            s.input.erase(prev, s.cursor_byte - prev);
            s.cursor_byte = prev;
        }
        break;
    case Key::delete_:
        if (s.cursor_byte < s.input.size()) {
            auto next = next_codepoint(s.input, s.cursor_byte);
            s.input.erase(s.cursor_byte, next - s.cursor_byte);
        }
        break;
    case Key::left:
        s.cursor_byte = previous_codepoint(s.input, s.cursor_byte);
        break;
    case Key::right:
        s.cursor_byte = next_codepoint(s.input, s.cursor_byte);
        break;
    case Key::home:
        s.cursor_byte = 0;
        break;
    case Key::end:
        s.cursor_byte = s.input.size();
        break;
    default:
        break;
    }
}

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

std::string text_delta(
    const nxt::io::llm::stream_event & event,
    std::string_view type)
{
    if (event.type != type)
        return {};
    return event.payload.value("delta", std::string{});
}

auto input_field(const state & s)
{
    auto input = s.input;
    auto cursor_byte = s.cursor_byte;
    auto busy = s.busy;

    return leaf(
        WidthHint::grow(),
        HeightHint::fixed(1 * ln),
        [=](RasterView & r, Size size) {
            if (size.w.count() == 0)
                return;

            const auto bg = busy ? Rgba8(38, 40, 44) : Rgba8(42, 47, 54);
            const auto fg = busy ? Rgba8(150, 156, 162) : Rgba8(230, 235, 240);
            for (std::size_t x = 0; x < size.w.count(); ++x) {
                auto pos = Pos::at(x * ch, 0 * ln);
                r.set_bg(pos, bg);
                r.set_fg(pos, fg);
            }

            auto prefix = std::string{"> "};
            r.write_text(Pos::origin(), prefix);

            auto prefix_w = prefix.size();
            auto field_w =
                size.w.count() > prefix_w ? size.w.count() - prefix_w : 0;
            auto cursor_cell = cell_at_byte(input, cursor_byte);
            auto total_cells = codepoint_count(input);
            auto scroll_cell = cursor_cell >= field_w && field_w > 0
                ? cursor_cell - field_w + 1
                : std::size_t{0};
            auto start_byte = byte_at_cell(input, scroll_cell);
            auto end_byte =
                byte_at_cell(std::string_view{input}.substr(start_byte), field_w)
                + start_byte;
            auto visible = input.substr(start_byte, end_byte - start_byte);
            auto visible_cursor = cursor_cell - scroll_cell;

            r.write_text(Pos::at(prefix_w * ch, 0 * ln), visible);

            auto cursor_x = std::min(prefix_w + visible_cursor, size.w.count() - 1);
            r.set_em(Pos::at(cursor_x * ch, 0 * ln), Emphasis::reverse);
            if (cursor_cell == total_cells)
                r.set_char(Pos::at(cursor_x * ch, 0 * ln), ' ');
        });
}

auto chat_hud(const state & s)
{
    auto accent = fg(Rgba8{90, 190, 210}) | bold;
    auto muted = fg(Rgba8{150, 156, 162});
    auto bad = fg(Rgba8{235, 120, 120}) | bold;
    auto status_style = s.error.empty() ? fg(Rgba8{220, 224, 228}) : bad;
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

        auto on_event = [&](nxt::io::llm::stream_event event) -> nxt::task<> {
            auto delta = text_delta(event, "response.output_text.delta");
            if (!delta.empty()) {
                reply += delta;
                runtime.print(delta);
            }
            co_return;
        };

        co_await nxt::io::llm::stream_openai_responses_over(
            transport,
            request,
            on_event,
            runtime.get_stop_token());
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

            auto question = std::move(s.input);
            s.input.clear();
            s.cursor_byte = 0;
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

        apply_edit(s, *event);
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
