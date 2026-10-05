// Offline Responses WebSocket consumer: verified TLS, no real credentials.
#include <nxtai/agent.hpp>
#include <nxtai/responses_websocket_transport.hpp>
#include <nxtrt/arch.hpp>

#include <cstdio>
#include <tuple>

using namespace nxtrt;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char * why)
{
    if (!ok)
        throw runtime_error{why};
}

struct observer
{
    std::string output = {};
    unsigned tools = 0;
    bool fail = false;

    task<> text(std::string delta)
    {
        if (fail)
            throw runtime_error{"observer failure"};
        output += delta;
        co_return;
    }

    task<> tool_started(const nxtai::tools::function_call &)
    {
        ++tools;
        co_return;
    }

    task<> tool_finished(const nxtai::tools::function_call_result &)
    {
        co_return;
    }
};

nxtai::responses::openai_responses_request request()
{
    return {
        .api_key = "fixture-key",
        .model = "fixture-model",
        .input = "inspect"};
}

task<> agent(nxtai::responses_websocket_options options)
{
    auto transport = nxtai::responses_websocket_transport{options};
    auto display = observer{};
    // Unknown tools deliberately produce a failed tool result: it must be
    // returned with its call id, without replaying the prompt or reasoning.
    auto registry = nxtai::tools::tool_registry{};
    auto input = request();
    input.tools = {
        {.name = "missing",
         .description = "fixture tool",
         .parameters = {R"({"type":"object"})"}}};
    input.include = {"reasoning.encrypted_content"};
    input.max_output_tokens = 731;
    input.reasoning_effort = "low";
    input.reasoning_summary = "auto";
    co_await nxtai::run_agent(input, registry, transport, display);
    check(
        display.output == "first answer" && display.tools == 2,
        "agent result mismatch");

    // A changed key must not silently use the earlier authentication, nor
    // damage the established connection when rejected locally.
    auto next = request();
    next.api_key = "different-key";
    bool rejected = false;
    try {
        (void) co_await transport(next, display);
    } catch (invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "credential change accepted");
    next.api_key = "fixture-key";
    next.input = "fresh chain";
    auto fresh = co_await transport(next, display);
    check(
        fresh.id == "resp_fresh", "new chain inherited previous response");
    check(
        fresh.output_items.at(0).str.find("final_answer")
            != std::string::npos,
        "canonical output lost unknown fields");
}

task<>
failed(nxtai::responses_websocket_options options, std::string expected)
{
    auto transport = nxtai::responses_websocket_transport{options};
    auto display = observer{.fail = expected == "observer failure"};
    bool rejected = false;
    try {
        (void) co_await transport(request(), display);
    } catch (runtime_error const & e) {
        rejected =
            std::string{e.what()}.find(expected) != std::string::npos;
    }
    check(rejected, "wrong Responses failure");
    bool terminal = false;
    try {
        (void) co_await transport(request(), display);
    } catch (logic_error const &) {
        terminal = true;
    }
    check(terminal, "failed session was reused or automatically retried");
}

task<> overlap(
    nxtai::responses_websocket_transport & transport, observer & display)
{
    // Wait for a real server delta, not a guess about handshake timing.
    while (display.output != "parked")
        co_await op::timeout::after(1ms);
    bool rejected = false;
    try {
        (void) co_await transport(request(), display);
    } catch (logic_error const & e) {
        rejected =
            std::string{e.what()}.find("overlapping") != std::string::npos;
    }
    check(rejected, "overlapping turn accepted");
    co_await op::timeout::after(20ms);
}

task<> cancellation(nxtai::responses_websocket_options options)
{
    auto transport = nxtai::responses_websocket_transport{options};
    auto display = observer{};
    auto [work, stopped] = co_await settle(
        std::tuple{
            transport(request(), display), overlap(transport, display)},
        first_completion_group{});
    check(
        stopped && !work && is_operation_cancelled(work.error()),
        "Responses cancellation did not drain");
    bool terminal = false;
    try {
        (void) co_await transport(request(), display);
    } catch (logic_error const &) {
        terminal = true;
    }
    check(terminal, "cancelled session was reused");
    options.url.replace(
        options.url.rfind('/'), std::string::npos, "/agent");
    co_await agent(
        options); // Same deck after cancellation, with fresh state.
}

task<> invalid_inputs()
{
    bool rejected = false;
    try {
        auto transport =
            nxtai::responses_websocket_transport{{.url = "ws://unused/"}};
    } catch (invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "plaintext OpenAI credentials allowed");
    for (auto key :
         {std::string{},
          std::string{"bad\r\nInjected: yes"},
          std::string{"x\0y", 3}}) {
        auto transport = nxtai::responses_websocket_transport{};
        auto display = observer{};
        auto input = request();
        input.api_key = key;
        rejected = false;
        try {
            (void) co_await transport(input, display);
        } catch (invalid_argument const &) {
            rejected = true;
        }
        check(rejected, "invalid API key accepted");
    }
    for (auto header : std::vector<nxtrt::http::header>{
             {"bad name", "value"},
             {"Authorization", "x\r\nInjected: yes"},
             {"Authorization", std::string{"x\0y", 3}},
             {"hOsT", "evil"},
             {"Connection", "close"},
             {"Upgrade", "other"},
             {"Content-Length", "99"},
             {"Transfer-Encoding", "chunked"},
             {"sEc-WeBsOcKeT-Key", "override"}}) {
        auto config =
            websocket::options{.ca_file = {}, .headers = {header}};
        rejected = false;
        try {
            (void) co_await websocket::connect(
                "ws://unused.invalid/", config);
        } catch (invalid_argument const &) {
            rejected = true;
        }
        check(rejected, "unsafe Upgrade header accepted");
    }
}
} // namespace

int main(int argc, char ** argv)
{
    if (argc != 3)
        return 2;
    auto failures = 0;
    auto test = [&](const char * name, task<> work) {
        try {
            auto wand = arch::wand{};
            auto d = deck{&wand};
            auto root = root_task{
                d, [&] { return with_timeout(10s, std::move(work)); }};
            root.start();
            wand.run_until_done(d, root.inner());
            std::move(root.inner()).result();
            std::printf("PASS %s\n", name);
        } catch (std::exception const & e) {
            ++failures;
            std::printf("FAIL %s: %s\n", name, e.what());
        }
    };
    test("local input validation", invalid_inputs());
    auto config = nxtai::responses_websocket_options{
        .url = std::string{argv[1]} + "/agent", .ca_file = argv[2]};
    test(
        "store=false incremental agent and fresh chain on one connection",
        agent(config));
    for (auto [path, expected] :
         {std::pair{"failed", "response.failed"},
          {"incomplete", "response.incomplete"},
          {"error", "previous_response_not_found"},
          {"close", "ended before completion"},
          {"binary", "expected WebSocket text"},
          {"malformed", "trailing Responses JSON"},
          {"missing-type", "missing type"},
          {"observer", "observer failure"}}) {
        config.url = std::string{argv[1]} + "/" + path;
        test(path, failed(config, expected));
    }
    config.url = std::string{argv[1]} + "/stall";
    test(
        "overlap rejection, cancellation, drain and fresh session",
        cancellation(config));
    std::printf("RESPONSES WEBSOCKET: %d failures\n", failures);
    return failures ? 1 : 0;
}
