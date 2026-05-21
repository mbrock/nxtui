#include <nxt/text_field.hpp>
#include <nxt/tui.hpp>
#include <nxtai/agent_tools.hpp>
#include <nxtai/builtin_tools.hpp>
#include <nxtai/hud_blocks.hpp>
#include <nxtai/response_turn.hpp>
#include <nxtai/responses.hpp>
#include <nxtai/tool_ui.hpp>
#include <nxtio/input.hpp>
#include <nxtio/process.hpp>
#include <nxtio/text_field.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
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
    bool store = true;
    // When set, run one agent turn against this prompt with tools
    // enabled and exit. Otherwise open the interactive HUD.
    std::optional<std::string> oneshot_prompt;
};

cli_options parse_args(int argc, char ** argv)
{
    auto options = cli_options{};
    auto positionals = std::vector<std::string>{};

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--model" || arg == "-m") {
            if (i + 1 >= argc)
                throw std::runtime_error{"--model requires a value"};
            options.model = argv[++i];
            continue;
        }
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
        if (arg == "--stateless") {
            options.store = false;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: nxtllm [options] [prompt...]\n"
                   "  with prompt:            runs one agent turn with tools, then exits\n"
                   "  without prompt:         opens the interactive HUD (TTY only)\n"
                   "\n"
                   "  -m, --model MODEL                 (default: gpt-5.4-mini)\n"
                   "  --max-output-tokens N\n"
                   "  --reasoning-effort minimal|low|medium|high|xhigh\n"
                   "  --reasoning-summary auto|concise|detailed|none\n"
                   "  --stateless                       resend local transcript instead of using stored response ids\n"
                   "\n"
                   "  NXT_TRACE=auto         emit an Arrow trace under traces/\n";
            std::exit(EXIT_SUCCESS);
        }
        positionals.emplace_back(arg);
    }

    // Join all positionals into the one-shot prompt so callers can
    // write `nxtllm what is in the readme` without quoting. Quoting
    // still works — quoted args land as single positionals.
    if (!positionals.empty()) {
        auto joined = std::string{};
        for (std::size_t i = 0; i < positionals.size(); ++i) {
            if (i > 0)
                joined += ' ';
            joined += positionals[i];
        }
        options.oneshot_prompt = std::move(joined);
    }

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
        .store = options.store,
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

void queue_post_exit_summary(
    nxt::ui::yard & self,
    const nxt::ai::hud_blocks::State & hud,
    const nxt::ai::agent::response_stream_result & response)
{
    auto block = std::string{};
    auto wrap_width = nxt::ai::response_turn::stream_wrap_width(self);
    auto message_style = nxt::tui::fg(nxt::Rgba8::yellow());

    for (const auto & message :
         nxt::ai::agent::message_blocks_from_items(response.output_items)) {
        if (message.empty())
            continue;
        if (!block.empty())
            block += "\n";
        block += nxt::ai::tool_ui::render_for_scrollback(
            self,
            nxt::ai::response_turn::markdown_text_block(
                message, message_style, wrap_width));
        block += "\n";
    }

    if (!hud.rows.empty()) {
        if (!block.empty())
            block += "\n";
        block += nxt::ai::tool_ui::render_for_scrollback(self, hud.view());
        block += "\n";
    }

    if (!block.empty()) {
        block.insert(block.begin(), '\n');
        self.runtime().print_after_exit(std::move(block));
    }
}

template<typename ToolSet>
nxt::task<> run_agent_worker(
    nxt::ui::yard & self,
    nxt::ai::agent::response_continuation<ToolSet> turn,
    bool post_exit_summary = false)
{
    auto hud = nxt::ai::hud_blocks::State{};
    while (turn.can_step()) {
        self.draw(
            hud.view(nxt::ai::response_turn::status_row("preparing request")));
        auto response =
            co_await nxt::ai::response_turn::request_response_turn(
                self, turn.request(), &hud);

        if (self.cancelled())
            co_return;
        auto calls =
            nxt::ai::tools::function_calls_from_items(response.output_items);
        if (calls.empty()) {
            if (post_exit_summary)
                queue_post_exit_summary(self, hud, response);
            co_return;
        }

        if (auto problem = turn.continuation_problem(response, !calls.empty())) {
            self.println(*problem);
            co_return;
        }

        auto outputs = co_await nxt::ai::tool_ui::run_all(
            self, turn.tools(), calls, &hud);

        turn.continue_after_tools(std::move(response), std::move(outputs));
    }

    self.println("too much");
}

template<typename ToolSet>
nxt::task<> run_agent_turn(
    nxt::ui::yard & self,
    nxt::ai::agent::response_continuation<ToolSet> turn,
    bool post_exit_summary = false)
{
    co_await run_agent_worker(self, std::move(turn), post_exit_summary);
}

nxt::task<> run_submitted_prompt(
    nxt::ui::yard & self,
    llm_request request,
    bool post_exit_summary = false)
{
    if (!prepare_api_key(request)) {
        self.println("error: OPENAI_API_KEY is not set");
        co_return;
    }

    auto tools = nxt::ai::tools::concat(
        nxt::ai::builtin_tools::for_runtime(self.runtime()),
        nxt::ai::agent_tools::for_agent(self.runtime().scheduler()));
    co_await run_agent_turn(
        self,
        nxt::ai::agent::response_continuation{
            std::move(request), std::move(tools)},
        post_exit_summary);
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

        if (options.oneshot_prompt) {
            // One-shot mode: run a single agent turn (LLM + tools) and
            // exit. This works whether stdout is a TTY or not; the
            // HUD is unused but `self.println` still routes output to
            // stdout. Pair with `NXT_TRACE=auto` to capture the full
            // request/response/tool flow for inspection.
            request.input = std::move(*options.oneshot_prompt);
            return nxt::ui::run2(
                [request = std::move(request)](
                    nxt::ui::yard & self) mutable -> nxt::task<> {
                    co_await run_submitted_prompt(
                        self, std::move(request), true);
                });
        }

        return nxt::ui::run2(
            [request = std::move(request)](
                nxt::ui::yard & self) mutable -> nxt::task<> {
                if (self.runtime().has_terminal_surface()) {
                    co_await run_prompt_loop(self, std::move(request));
                } else {
                    self.println(
                        "error: nxtllm without a prompt requires a TTY;\n"
                        "pass a prompt as positional args for one-shot mode");
                }
            });

    } catch (const std::exception & e) {
        std::cerr << "nxtllm error: " << e.what() << '\n';
        return 1;
    }
}
