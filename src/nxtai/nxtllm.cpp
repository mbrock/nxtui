#include <nxt/text_field.hpp>
#include <nxt/tui.hpp>
#include <nxtai/builtin_tools.hpp>
#include <nxtai/response_turn.hpp>
#include <nxtai/responses.hpp>
#include <nxtio/input.hpp>
#include <nxtio/process.hpp>
#include <nxtio/text_field.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using llm_request = nxt::ai::responses::openai_responses_request;

constexpr std::size_t default_max_output_tokens = 128000;

struct cli_options
{
    std::string model = "gpt-5.4-mini";
    std::size_t max_output_tokens = default_max_output_tokens;
    std::string reasoning_effort = "medium";
    std::string reasoning_summary = "auto";
};

cli_options parse_args(int argc, char ** argv)
{
    auto options = cli_options{};
    auto positionals = std::vector<std::string>{};

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--max-output-tokens") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--max-output-tokens requires a value"};
            options.max_output_tokens = std::stoull(argv[++i]);
            continue;
        }
        if (arg == "--reasoning-effort") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--reasoning-effort requires a value"};
            options.reasoning_effort = argv[++i];
            continue;
        }
        if (arg == "--reasoning-summary") {
            if (i + 1 >= argc)
                throw std::runtime_error{
                    "--reasoning-summary requires a value"};
            options.reasoning_summary = argv[++i];
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: nxtllm [options] [model]\n"
                   "  stdout TTY              opens the prompt HUD\n"
                   "  redirected stdout       runs one prompt in plain CLI mode\n"
                   "  --max-output-tokens N\n"
                   "  --reasoning-effort minimal|low|medium|high|xhigh\n"
                   "  --reasoning-summary auto|concise|detailed|none\n";
            std::exit(EXIT_SUCCESS);
        }
        positionals.emplace_back(arg);
    }

    if (positionals.size() > 0)
        options.model = std::move(positionals[0]);
    if (positionals.size() > 1)
        throw std::runtime_error{"too many positional arguments"};

    return options;
}

llm_request make_request(const cli_options & options, std::string api_key)
{
    return llm_request{
        .api_key = std::move(api_key),
        .model = options.model,
        .input = "",
        .previous_response_id = {},
        .max_output_tokens = options.max_output_tokens,
        .reasoning_effort = options.reasoning_effort,
        .reasoning_summary = options.reasoning_summary == "none"
                                 ? std::string{}
                                 : options.reasoning_summary,
    };
}

const auto coolstyle = nxt::tui::TextFieldStyle{
    .fg = nxt::Rgba8{220, 224, 228},
    .bg = nxt::Rgba8{28, 32, 36},
    .prefix_fg = nxt::Rgba8{90, 190, 210},
    .placeholder_fg = nxt::Rgba8{105, 110, 118},
};

bool prepare_api_key(llm_request & request)
{
    auto api_key = std::getenv("OPENAI_API_KEY");
    if (api_key != nullptr && !std::string_view{api_key}.empty()) {
        request.api_key = api_key;
        return true;
    }

    return false;
}

nxt::task<> run_agent_worker(
    nxt::ui::yard & self, nxt::ai::agent::response_continuation turn)
{
    while (turn.can_step()) {
        self.draw(nxt::tui::text("Requesting..."));
        auto response =
            co_await nxt::ai::response_turn::request_response_turn(
                self, turn.request());

        if (self.cancelled())
            co_return;
        if (response.done())
            co_return;

        if (auto problem = turn.continuation_problem(response)) {
            self.println(*problem);
            co_return;
        }

        self.draw(nxt::tui::text("Tooling..."));
        auto outputs = co_await nxt::ai::response_turn::run_requested_tools(
            self, turn.tools(), response.tool_calls());

        turn.continue_after_tools(std::move(response), std::move(outputs));
    }

    self.println("too much");
}

nxt::task<> run_agent_turn(
    nxt::ui::yard & self, nxt::ai::agent::response_continuation turn)
{
    co_await nxt::ui::spintag(self, [&](nxt::ui::yard & s) -> nxt::task<> {
        co_await run_agent_worker(s, std::move(turn));
    });
}

nxt::task<> run_submitted_prompt(nxt::ui::yard & self, llm_request request)
{
    if (!prepare_api_key(request)) {
        self.println("error: OPENAI_API_KEY is not set");
        co_return;
    }

    auto tools = nxt::ai::builtin_tools::for_runtime(self.runtime());
    co_await run_agent_turn(
        self,
        nxt::ai::agent::response_continuation{
            std::move(request), std::move(tools)});
}

nxt::task<> run_prompt_loop(nxt::ui::yard & self, llm_request request)
{
    auto input = nxt::tui::TextField{};
    auto draw_input = [&] {
        self.draw(
            nxt::tui::text_field(
                input,
                nxt::tui::TextFieldOptions{
                    .prefix = request.model + "> ",
                    .placeholder = "Type something...",
                    .style = coolstyle,
                }));
    };

    draw_input();

    while (!self.cancelled()) {
        auto event = co_await nxt::ui::next_key_press(self);
        if (!event)
            co_return;

        if (nxt::ui::is_escape(*event)) {
            self.request_shutdown();
            co_return;
        }

        if (event->key == nxt::input::Key::enter) {
            if (input.empty())
                continue;

            auto text = std::move(input.text);
            input.clear();
            request.input = std::move(text);

            co_await run_submitted_prompt(self, request);
            draw_input();
            continue;
        }

        if (nxt::tui::apply_key(input, *event))
            draw_input();
    }
}

} // namespace

int main(int argc, char ** argv)
{
    try {
        auto options = parse_args(argc, argv);
        auto request = make_request(options, {});

        auto status = nxt::ui::run2(
            [request = std::move(request)](
                nxt::ui::yard & self) mutable -> nxt::task<> {
                if (self.runtime().has_terminal_surface()) {
                    co_await run_prompt_loop(self, std::move(request));
                }
            });
        return status;

    } catch (const std::exception & e) {
        std::cerr << "nxtllm error: " << e.what() << '\n';
        return 1;
    }
}
