#include <nxt/rt/app.hpp>
#include <nxt/rt/task.hpp>
#include <nxt/rt/trace.hpp>
#include <nxt/rt/ui_runtime.hpp>
#include <nxt/llm/common.hpp>
#include <nxt/llm/agent_tools.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using nxt::llm::llm_request;
using nxt::llm::default_max_output_tokens;
using nxt::llm::frame_interval;

struct cli_options
{
    std::string model = "gpt-5.4-mini";
    std::size_t max_output_tokens = default_max_output_tokens;
    std::string reasoning_effort = "medium";
    std::string reasoning_summary = "auto";
    bool store = true;
    bool dump_request = false;
    std::optional<std::string> oneshot_prompt;
};

[[noreturn]] void print_help_and_exit()
{
    std::cout
        << "usage: nxtllm [options] [prompt...]\n"
           "  ng migration build: streams one-shot Responses text on nxt::rt\n"
           "\n"
           "  -m, --model MODEL                 (default: gpt-5.4-mini)\n"
           "  --max-output-tokens N\n"
           "  --reasoning-effort none|low|medium|high|xhigh\n"
           "  --reasoning-summary auto|concise|detailed|none\n"
           "  --stateless                       resend local transcript instead of using stored response ids\n"
           "  --dump-request                    print serialized Responses JSON\n";
    std::exit(EXIT_SUCCESS);
}

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
        if (arg == "--dump-request") {
            options.dump_request = true;
            continue;
        }
        if (arg == "--help" || arg == "-h")
            print_help_and_exit();

        positionals.emplace_back(arg);
    }

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

std::string env_string(const char * name)
{
    auto * value = std::getenv(name);
    if (value == nullptr)
        return {};
    return value;
}

llm_request make_request(const cli_options & options)
{
    return llm_request{
        .api_key = env_string("OPENAI_API_KEY"),
        .model = options.model,
        .input = options.oneshot_prompt.value_or(""),
        .previous_response_id = {},
        .max_output_tokens = options.max_output_tokens,
        .reasoning_effort = options.reasoning_effort,
        .reasoning_summary = options.reasoning_summary == "none"
                                 ? std::string{}
                                 : options.reasoning_summary,
        .store = options.store,
    };
}

nxt::rt::task<int> run_nxtllm(cli_options options)
{
    auto request = make_request(options);
    auto tools = nxt::llm::agent_tools::for_agent();

    if (!options.oneshot_prompt) {
        std::cout
            << "nxtllm is now on nxt::rt; the interactive HUD is still being "
               "ported.\n"
            << "Pass a prompt for one-shot request construction, or use "
               "--dump-request to inspect the JSON envelope.\n";
        co_return EXIT_SUCCESS;
    }

    nxt::llm::prepare_tool_request(request, tools);

    if (options.dump_request) {
        std::cout << nxt::llm::responses::openai_responses_body(request)
                  << '\n';
        co_return EXIT_SUCCESS;
    }

    if (request.api_key.empty()) {
        std::cerr
            << "nxtllm: OPENAI_API_KEY is not set; streaming is wired through "
               "nxt::rt, but it needs credentials.\n"
            << "Try --dump-request to inspect the ng Responses payload.\n";
        co_return EXIT_FAILURE;
    }

    auto trace = std::make_shared<nxt::rt::trace_context>();
    auto root_span = trace->start_span(
        "nxtllm.request",
        {},
        {{"model", request.model}});

    try {
        co_await nxt::rt::with_env<nxt::rt::trace_context_key>(
            trace,
            [&]() mutable {
                return nxt::rt::with_env<nxt::rt::trace_current_span_key>(
                    root_span.span_id(),
                    [&]() mutable {
                        return nxt::rt::with_ui_zone(
                            [request = std::move(request),
                             tools = std::move(tools)]() mutable {
                                return nxt::llm::run_agent_ui_zone(
                                    std::move(request),
                                    std::move(tools));
                            },
                            {},
                            frame_interval);
                    });
            });
        root_span.finish("ok");
    } catch (...) {
        root_span.finish("error");
        throw;
    }

    co_return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char ** argv)
try {
    auto rt = nxt::rt::runtime{};
    return rt.run(run_nxtllm(parse_args(argc, argv)));
} catch (std::exception const & error) {
    std::cerr << "nxtllm: " << error.what() << '\n';
    return EXIT_FAILURE;
}
