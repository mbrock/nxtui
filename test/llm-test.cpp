#include <nxtai/agent_tools.hpp>
#include <nxtai/responses.hpp>
#include <nxtai/response_turn.hpp>
#include <nxtai/tool_ui.hpp>
#include <nxtai/tools.hpp>
#include <nxtai/tools/subprocess.hpp>

#include <boost/ut.hpp>
#include <glaze/json/json_ptr.hpp>

#include <array>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

template<typename T, glz::string_literal Path>
T json_at(std::string_view json)
{
    auto value = glz::get_as_json<T, Path>(json);
    expect(bool(value));
    return *value;
}

template<glz::string_literal Path>
bool json_has(std::string_view json)
{
    return bool(glz::get_sv_json<Path>(json));
}

template<typename Layout>
std::vector<std::string> render_lines(const Layout & layout)
{
    auto width = std::max<std::size_t>(
        1, static_cast<std::size_t>(layout.width_hint().min.count()));
    auto height = std::max<std::size_t>(
        1, static_cast<std::size_t>(layout.height_hint().min.count()));

    GlyphTable glyphs;
    Raster raster(width * ch, height * ln, glyphs);
    auto view = raster.view();
    layout.render(view, Size{width * ch, height * ln});

    auto out = std::vector<std::string>{};
    for (auto row = std::size_t{0}; row < height; ++row) {
        auto line = std::string{};
        for (auto x = std::size_t{0}; x < width; ++x) {
            auto cell = view.get_cell(Pos::at(x * ch, row * ln));
            if (cell) {
                if (auto glyph = raster.glyph_table().get(cell->glyph))
                    line += *glyph;
                else
                    line += ' ';
            } else {
                line += ' ';
            }
        }
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

template<typename Layout>
std::vector<std::string>
render_lines(const Layout & layout, std::size_t width, std::size_t height)
{
    GlyphTable glyphs;
    Raster raster(width * ch, height * ln, glyphs);
    auto view = raster.view();
    layout.render(view, Size{width * ch, height * ln});

    auto out = std::vector<std::string>{};
    for (auto row = std::size_t{0}; row < height; ++row) {
        auto line = std::string{};
        for (auto x = std::size_t{0}; x < width; ++x) {
            auto cell = view.get_cell(Pos::at(x * ch, row * ln));
            if (cell) {
                if (auto glyph = raster.glyph_table().get(cell->glyph))
                    line += *glyph;
                else
                    line += ' ';
            } else {
                line += ' ';
            }
        }
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

template<typename Layout>
Emphasis rendered_emphasis(const Layout & layout, std::size_t row, std::size_t col)
{
    auto width = std::max<std::size_t>(
        1, static_cast<std::size_t>(layout.width_hint().min.count()));
    auto height = std::max<std::size_t>(
        1, static_cast<std::size_t>(layout.height_hint().min.count()));

    GlyphTable glyphs;
    Raster raster(width * ch, height * ln, glyphs);
    auto view = raster.view();
    layout.render(view, Size{width * ch, height * ln});

    auto cell = view.get_cell(Pos::at(col * ch, row * ln));
    return cell ? cell->em : Emphasis::none;
}

struct test_echo_tool
{
    static constexpr std::string_view name = "nxt_echo";
    static constexpr std::string_view description = "Echo text.";
    static constexpr bool strict = true;
    static constexpr std::string_view icon = "echo";

    struct parameters
    {
        std::string text;

        struct glaze_json_schema
        {
            glz::schema text{.description = "Text to echo."};
        };
    };

    static std::string parameters_summary(const parameters & args)
    {
        return args.text;
    }

    static nxt::Rgba8 theme_color(const nxt::theme::Palette & palette)
    {
        return palette.green;
    }

    nxt::task<nxt::ai::tools::tool_result> run(parameters args) const
    {
        co_return nxt::ai::tools::tool_result{
            .output = std::move(args.text),
        };
    }
};

struct test_empty_tool
{
    static constexpr std::string_view name = "nxt_empty";
    static constexpr std::string_view description = "No arguments.";
    static constexpr bool strict = true;

    struct parameters
    {
    };

    nxt::task<nxt::ai::tools::tool_result> run(parameters) const
    {
        co_return nxt::ai::tools::tool_result{};
    }
};

static suite llm_tests = [] {
    using namespace std::literals;

    "openai responses request serializes response body"_test = [] {
        auto request = nxt::ai::responses::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "Say ok.",
            .input_items = {},
            .tools = {},
            .include = {},
            .previous_response_id = {},
            .max_output_tokens = 64,
            .reasoning_effort = "minimal",
            .reasoning_summary = "auto",
            .store = false,
        };

        auto wire =
            nxt::http::serialize(
                nxt::ai::responses::openai_responses_http_request(request));
        auto body_start = wire.find("\r\n\r\n");
        expect(body_start != std::string::npos);

        auto body = wire.substr(body_start + 4);
        expect(json_at<std::string, "/model">(body) == "gpt-5-mini");
        expect(json_at<std::string, "/input">(body) == "Say ok.");
        expect(json_at<bool, "/stream">(body));
        expect(!json_at<bool, "/store">(body));
        expect(json_at<std::string, "/reasoning/effort">(body) == "minimal");
        expect(json_at<std::string, "/reasoning/summary">(body) == "auto");
        expect(wire.find("Authorization: Bearer test-key\r\n") != std::string::npos);
    };

    "openai responses request serializes tools and function outputs"_test = [] {
        auto request = nxt::ai::responses::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "ignored for tool output turn",
            .input_items =
                {nxt::ai::tools::function_call_output(
                    "call_123",
                    "{\"ok\":true}")},
            .tools = {nxt::ai::tools::function_tool_definition(
                test_echo_tool{})},
            .include = {"reasoning.encrypted_content"},
            .previous_response_id = "resp_123",
            .max_output_tokens = 64,
            .reasoning_effort = "medium",
            .reasoning_summary = "",
            .store = true,
        };

        auto body = nxt::ai::responses::openai_responses_body(request);
        expect(json_at<std::string, "/input/0/type">(body) ==
               "function_call_output");
        expect(json_at<std::string, "/input/0/call_id">(body) == "call_123");
        expect(json_at<std::string, "/tools/0/name">(body) == "nxt_echo");
        expect(json_at<std::string, "/include/0">(body) ==
               "reasoning.encrypted_content");
        expect(json_at<std::string, "/previous_response_id">(body) ==
               "resp_123");
        expect(json_at<bool, "/store">(body));
    };

    "function tool definition derives parameter json schema"_test = [] {
        auto definition =
            glz::ex::write_json(
                nxt::ai::tools::function_tool_definition(test_echo_tool{}));

        expect(json_at<std::string, "/type">(definition) == "function");
        expect(json_at<std::string, "/name">(definition) == "nxt_echo");
        expect(json_at<std::string, "/parameters/type">(definition) ==
               "object");
        expect(!json_has<"/parameters/title">(definition));
        expect(!json_at<bool, "/parameters/additionalProperties">(definition));
        expect(json_at<std::string, "/parameters/required/0">(definition) ==
               "text");
        expect(json_at<std::string, "/parameters/properties/text/type">(
                   definition) == "string");
        expect(json_at<std::string,
                       "/parameters/properties/text/description">(
                   definition) == "Text to echo.");

        auto empty_definition =
            glz::ex::write_json(
                nxt::ai::tools::function_tool_definition(test_empty_tool{}));
        auto properties =
            glz::get_sv_json<"/parameters/properties">(empty_definition);
        expect(bool(properties));
        expect(*properties == "{}");
        auto required =
            glz::get_sv_json<"/parameters/required">(empty_definition);
        expect(bool(required));
        expect(*required == "[]");
    };

    "openai responses request builds stateless tool input history"_test = [] {
        auto request = nxt::ai::responses::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "Use a tool.",
            .input_items = {},
            .tools = {},
            .include = {"reasoning.encrypted_content"},
            .previous_response_id = {},
            .max_output_tokens = 64,
            .reasoning_effort = "medium",
            .reasoning_summary = "",
            .store = false,
        };

        auto input = nxt::ai::responses::input_items_from_request(request);
        input.emplace_back(
            R"({"id":"fc_123","type":"function_call","call_id":"call_123","name":"nxt_echo","arguments":"{\"text\":\"hello\"}"})");
        input.push_back(nxt::ai::tools::function_call_output(
            "call_123",
            "{\"text\":\"hello\"}"));

        request.input.clear();
        request.input_items = std::move(input);
        auto body = nxt::ai::responses::openai_responses_body(request);
        expect(!json_at<bool, "/store">(body));
        expect(json_at<std::string, "/include/0">(body) ==
               "reasoning.encrypted_content");
        expect(!json_has<"/previous_response_id">(body));
        expect(json_at<std::string, "/input/0/role">(body) == "user");
        expect(json_at<std::string, "/input/1/type">(body) ==
               "function_call");
        expect(json_at<std::string, "/input/2/type">(body) ==
               "function_call_output");
    };

    "function call item parses and runs matching tool"_test = [] {
        auto item = nxt::ai::openai::raw_json{
            R"({"id":"fc_123","type":"function_call","call_id":"call_123","name":"nxt_echo","arguments":"{\"text\":\"hello\"}"})"};
        auto call = nxt::ai::tools::function_call_from_item(item);
        expect(call.has_value());
        expect(call->call_id == "call_123");
        expect(call->name == "nxt_echo");

        auto tools = nxt::ai::tools::tool_set{test_echo_tool{}};

        auto output =
            nxt::sync_wait(nxt::ai::tools::run_function_tool(tools, *call));
        expect(!output.failed);
        expect(output.output == "hello");

        auto missing = *call;
        missing.name = "missing";
        auto error =
            nxt::sync_wait(nxt::ai::tools::run_function_tool(tools, missing));
        expect(error.failed);
        expect(error.output == "unknown tool");
    };

    "tool ui argument summaries use concrete tool parameters"_test = [] {
        auto tool_list = nxt::ai::tools::tool_set{
            nxt::ai::agent_tools::rg_search_tool{},
            nxt::ai::agent_tools::bash_tool{},
            test_echo_tool{},
        };

        auto rg = nxt::ai::tools::function_call{
            .id = {},
            .call_id = {},
            .name = "rg_search",
            .arguments = R"({"pattern":"needle","path":"src"})",
            .item = {},
        };
        expect(nxt::ai::tool_ui::args_summary(tool_list, rg) ==
               "/needle/ in src");
        auto rg_display = nxt::ai::tool_ui::display_for_call(tool_list, rg);
        expect(rg_display.icon == "grep"sv);
        expect(
            rg_display.color
            == nxt::Rgba8{nxt::theme::baltic_church.amber});
        auto rg_result = nxt::ai::tools::tool_result{
            .output = "one\ntwo\n",
        };
        expect(
            nxt::ai::tool_ui::result_summary(rg_result) == "8 bytes");

        auto echo = nxt::ai::tools::function_call{
            .id = {},
            .call_id = {},
            .name = "nxt_echo",
            .arguments = R"({"text":"hello"})",
            .item = {},
        };
        expect(nxt::ai::tool_ui::args_summary(tool_list, echo) == "hello");
        auto echo_display =
            nxt::ai::tool_ui::display_for_call(tool_list, echo);
        expect(echo_display.icon == "echo"sv);

        auto bash = nxt::ai::tools::function_call{
            .id = {},
            .call_id = {},
            .name = "bash",
            .arguments = R"({"command":"set -e\nprintf 'hi\tthere\n'"})",
            .item = {},
        };
        expect(nxt::ai::tool_ui::args_summary(tool_list, bash) ==
               "set -e printf 'hi there '");

        auto failed = nxt::ai::tools::tool_result{
            .failed = true,
            .output = "bad\r\nthing\tfailed",
        };
        expect(nxt::ai::tool_ui::result_summary(failed) ==
               "error: bad thing failed");
    };

    "agent subprocess drains fast command output at eof"_test = [] {
        auto scheduler = nxt::scheduler::make_unique();
        for (auto i = 0; i < 20; ++i) {
            auto output = nxt::sync_wait(scheduler->schedule(
                nxt::ai::agent_tools::run_subprocess_async(
                    *scheduler,
                    std::vector<std::string>{
                        "/bin/bash", "-c", "printf 'hello\\n'"})));
            expect(output == "hello\n");
        }
    };

    "response item projections derive messages and calls"_test = [] {
        auto items = std::vector<nxt::ai::openai::raw_json>{
            nxt::ai::openai::raw_json{
                R"({"id":"msg_123","type":"message","role":"assistant","status":"completed","content":[{"type":"output_text","text":"hello"},{"type":"output_text","text":" world"}]})"},
            nxt::ai::openai::raw_json{
                R"({"id":"fc_123","type":"function_call","call_id":"call_123","name":"nxt_echo","arguments":"{\"text\":\"hello\"}"})"},
        };

        auto messages = nxt::ai::agent::message_blocks_from_items(items);
        expect(messages.size() == 1_ul);
        expect(messages[0] == "hello world");

        auto calls = nxt::ai::tools::function_calls_from_items(items);
        expect(calls.size() == 1_ul);
        expect(calls[0].call_id == "call_123");
        expect(calls[0].name == "nxt_echo");
    };

    "response text wrapping preserves paragraph breaks"_test = [] {
        auto lines = nxt::ai::response_turn::wrap_stream_text(
            "alpha beta\n\nsecond paragraph", 80);

        expect(
            lines
            == std::vector<std::string>{
                "alpha beta",
                "",
                "second paragraph"});
    };

    "response text wrapping uses terminal cell widths"_test = [] {
        auto lines = nxt::ai::response_turn::wrap_stream_text(
            "aa \xe7\x95\x8c bb", 5);

        expect(
            lines
            == std::vector<std::string>{"aa \xe7\x95\x8c", "bb"});
    };

    "response text wrapping indents markdown list continuations"_test = [] {
        auto lines = nxt::ai::response_turn::wrap_stream_text(
            "- alpha beta gamma", 10);

        expect(
            lines
            == std::vector<std::string>{"- alpha", "  beta", "  gamma"});
    };

    "inline markdown parses bold and code spans"_test = [] {
        auto spans = nxt::ai::response_turn::parse_inline_markdown(
            "plain **bold** `code`", {});

        expect(spans.size() == 4_ul);
        expect(spans[0].text == "plain ");
        expect(spans[1].text == "bold");
        expect(has_emphasis(spans[1].style.em, Emphasis::bold));
        expect(spans[2].text == " ");
        expect(spans[3].text == "code");
        expect(!has_emphasis(spans[3].style.em, Emphasis::reverse));
    };

    "markdown bold paragraph strips markers without vertical padding"_test = [] {
        auto layout = nxt::ai::response_turn::markdown_text_block(
            "**Foo bar**", {}, 80);

        expect(layout.height_hint().min == 1 * ln);
        expect(
            render_lines(layout)
            == std::vector<std::string>{"Foo bar"});
        expect(has_emphasis(
            rendered_emphasis(layout, 0, 0), Emphasis::bold));
    };

    "finished thought block folds to heading"_test = [] {
        auto layout = nxt::ai::response_turn::folded_thought_block();

        expect(layout.height_hint().min == 1 * ln);
        expect(
            render_lines(layout)
            == std::vector<std::string>{"▸ thought     folded"});
    };

    "hud block state renders folded rows above active work"_test = [] {
        auto hud = nxt::ai::hud_blocks::State{};
        hud.add(nxt::ai::response_turn::folded_thought_block());

        auto layout = hud.view(nxt::tui::text("working"));

        expect(layout.height_hint().min == 2 * ln);
        expect(
            render_lines(layout)
            == std::vector<std::string>{"▸ thought     folded", "working"});
    };

    "hud block state tail-scrolls when constrained"_test = [] {
        auto hud = nxt::ai::hud_blocks::State{};
        hud.add(nxt::tui::text("old"));
        hud.add(nxt::tui::text("new"));

        auto layout = hud.view(nxt::tui::text("working"));

        expect(layout.height_hint().min == 3 * ln);
        expect(
            render_lines(layout, 20, 2)
            == std::vector<std::string>{"new", "working"});
    };

    "hud block state clips oversized newest row instead of hiding it"_test = [] {
        auto hud = nxt::ai::hud_blocks::State{};
        hud.add(nxt::tui::text("old"));

        auto layout = hud.view(nxt::tui::column(
            nxt::tui::text("active-1"),
            nxt::tui::text("active-2"),
            nxt::tui::text("active-3")));

        expect(layout.height_hint().min == 4 * ln);
        expect(
            render_lines(layout, 20, 2)
            == std::vector<std::string>{"active-1", "active-2"});
    };

    "response output item type reads unquoted json string"_test = [] {
        auto event = nxt::ai::responses::stream_event{
            .type = "response.output_item.added",
            .data =
                R"({"type":"response.output_item.added","item":{"id":"rs_1","type":"reasoning","summary":[]}})",
        };

        expect(nxt::ai::response_turn::output_item_type(event) == "reasoning");
    };

    "output separator is one horizontal rule line"_test = [] {
        auto layout = nxt::ai::response_turn::output_separator();

        expect(layout.height_hint().min == 1 * ln);
        expect(render_lines(layout) == std::vector<std::string>{"─"});
    };

    "finished tool result block folds to done card"_test = [] {
        auto layout = nxt::ai::tool_ui::folded_result_card(
            nxt::ai::tool_ui::tool_display{
                .icon = "grep",
                .color = nxt::Rgba8{nxt::theme::baltic_church.amber},
            },
            "/needle/ in .",
            std::chrono::milliseconds{80},
            "3 matches",
            false);

        expect(layout.height_hint().min == 1 * ln);
    };

    "openai responses stream emits raw sse json events"_test = [] {
        auto sse =
            "event: response.output_text.delta\n"
            "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Ok\"}\n"
            "\n"
            "event: response.completed\n"
            "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n"
            "\n"s;

        auto response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: "
            + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        auto chunks = std::array{std::string_view{response}};
        nxt::io::string_transport transport{std::span{chunks}};

        std::vector<nxt::ai::responses::stream_event> events;
        auto request = nxt::ai::responses::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "Say ok.",
            .input_items = {},
            .tools = {},
            .include = {},
            .previous_response_id = {},
            .max_output_tokens = 64,
            .reasoning_effort = "medium",
            .reasoning_summary = "",
            .store = false,
        };

        nxt::sync_wait([&]() -> nxt::task<> {
            auto stream = nxt::ai::responses::openai_response_stream<
                nxt::io::string_transport>{transport};
            co_await stream.connect(request);
            while (auto event = co_await stream.next())
                events.push_back(std::move(*event));
        }());

        expect(events.size() == 2_ul);
        expect(events[0].type == "response.output_text.delta");
        expect(events[0].data == "{\"type\":\"response.output_text.delta\",\"delta\":\"Ok\"}");
        expect(events[1].type == "response.completed");
        expect(transport.written().starts_with("POST /v1/responses HTTP/1.1\r\n"));
    };

    "openai responses stream leaves event data unparsed"_test = [] {
        auto sse =
            "event: response.output_text.delta\n"
            "data: not-json\n"
            "\n"
            "data: [DONE]\n"
            "\n"s;

        auto response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: "
            + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        auto chunks = std::array{std::string_view{response}};
        nxt::io::string_transport transport{std::span{chunks}};

        auto request = nxt::ai::responses::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "Say ok.",
            .input_items = {},
            .tools = {},
            .include = {},
            .previous_response_id = {},
            .max_output_tokens = 64,
            .reasoning_effort = "medium",
            .reasoning_summary = "",
            .store = false,
        };

        auto event = nxt::sync_wait([&]()
            -> nxt::task<std::optional<nxt::ai::responses::stream_event>> {
            auto stream = nxt::ai::responses::openai_response_stream<
                nxt::io::string_transport>{transport};
            co_await stream.connect(request);
            co_return co_await stream.next();
        }());

        expect(event.has_value());
        expect(event->type == "response.output_text.delta");
        expect(event->data == "not-json");
    };
};

} // namespace nxt::test
