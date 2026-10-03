#include <nxtai/agent.hpp>
#include <nxtrt/http.hpp>
#include "task-test.hpp"

namespace nxt::test {
namespace {
using namespace boost::ut;
using nxtai::openai::raw_json;

struct agent_observer
{
    std::string text_output;
    std::size_t started = 0;
    std::size_t finished = 0;
    nxtrt::task<void> text(std::string delta)
    {
        text_output += delta;
        co_return;
    }
    nxtrt::task<void> tool_started(const nxtai::tools::function_call &)
    {
        ++started;
        co_return;
    }
    nxtrt::task<void> tool_finished(const nxtai::tools::function_call_result &)
    {
        ++finished;
        co_return;
    }
};

const std::string agent_first_response = R"({"type":"response.completed","response":{"id":"resp_1","status":"completed","output":[{"type":"reasoning","id":"rs_1","encrypted_content":"opaque-state","summary":[]},{"type":"function_call","id":"fc_1","call_id":"call_1","name":"echo","arguments":"{\"text\":\"first\"}"},{"type":"function_call","id":"fc_2","call_id":"call_2","name":"missing","arguments":"{}"}]}})";
const std::string agent_final_response = R"({"type":"response.completed","response":{"id":"resp_2","status":"completed","output":[{"type":"message","phase":"final_answer","content":[{"type":"output_text","text":"answer"}]}]}})";

struct scripted_transport
{
    std::vector<nxtai::responses::openai_responses_request> requests = {};
    bool repeat_calls = false;
    bool truncate = false;

    nxtrt::task<nxtai::responses::response_result> operator()(
        const nxtai::responses::openai_responses_request & request,
        agent_observer & observer)
    {
        requests.push_back(request);
        auto wire = std::string{};
        auto event = [&](std::string_view type, std::string_view data) {
            wire += "event: " + std::string{type} + "\ndata: " + std::string{data} + "\n\n";
        };
        // Incomplete argument strings are presentation events, not calls.
        event("response.function_call_arguments.delta",
            R"({"type":"response.function_call_arguments.delta","item_id":"fc_2","delta":"{"})");
        event("response.reasoning_summary_text.delta",
            R"({"type":"response.reasoning_summary_text.delta","delta":"thinking"})");
        if (requests.size() == 1 || repeat_calls) {
            if (!truncate)
                event("response.completed", agent_first_response);
        } else {
            event("response.output_text.delta",
                R"({"type":"response.output_text.delta","delta":"answer"})");
            event("response.completed", agent_final_response);
        }
        auto source = nxtrt::value_range_source{nxtrt::as_bytes(wire), 4096};
        auto events = nxtrt::http::sse_event_parser(source);
        auto decoder = nxtai::responses::stream_decoder{};
        while (auto next = co_await events.take()) {
            if (auto delta = co_await decoder.accept(next->type, next->data))
                co_await observer.text(std::move(*delta));
        }
        co_return decoder.finish();
    }
};

struct counting_echo
{
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Echo text";
    static constexpr bool strict = true;
    static constexpr std::string_view parameters_schema_json = R"({"type":"object"})";
    struct parameters { std::string text; };
    int * count;
    bool cancel = false;
    static std::optional<parameters> parse_parameters(std::string_view json)
    {
        auto text = nxtai::tools::json_string_member(json, "text");
        if (!text) return std::nullopt;
        return parameters{*text};
    }
    nxtrt::task<nxtai::tools::tool_result> run(parameters args) const
    {
        ++*count;
        if (cancel)
            throw nxtrt::operation_cancelled{};
        co_return nxtai::tools::tool_result{.output = args.text, .observed = std::nullopt};
    }
};

static suite ai_agent_tests{"AI agent", [] {
    "interleaved deltas stream while canonical items retain unknown fields"_test = []() -> nxtrt::task<void> {
        auto decoder = nxtai::responses::stream_decoder{};
        auto first = co_await decoder.accept("response.output_text.delta",
            R"({"type":"response.output_text.delta","output_index":2,"delta":"one"})");
        co_await decoder.accept("response.function_call_arguments.delta",
            R"({"type":"response.function_call_arguments.delta","output_index":1,"delta":"partial"})");
        auto second = co_await decoder.accept("response.refusal.delta",
            R"({"type":"response.refusal.delta","output_index":3,"delta":"two"})");
        expect(first == "one" && second == "two");
        co_await decoder.accept("response.completed", agent_final_response);
        auto result = decoder.finish();
        expect(result.output_items.size() == 1_ul);
        expect(result.output_items.at(0).str.find("final_answer") != std::string::npos);
    };
    "tool cancellation propagates without a continuation request"_test = []() -> nxtrt::task<void> {
        auto count = 0;
        auto tools = nxtai::tools::make_tool_registry({
            nxtai::tools::make_function_tool(counting_echo{&count, true})});
        auto transport = scripted_transport{};
        auto observer = agent_observer{};
        auto cancelled = false;
        try { co_await nxtai::run_agent({}, tools, transport, observer); }
        catch (const nxtrt::operation_cancelled &) { cancelled = true; }
        expect(cancelled);
        expect(count == 1);
        expect(transport.requests.size() == 1_ul);
        expect(observer.finished == 0_ul);
    };
    "stateless model tool model preserves reasoning and reports tool failures"_test = []() -> nxtrt::task<void> {
        auto count = 0;
        auto tools = nxtai::tools::make_tool_registry({
            nxtai::tools::make_function_tool(counting_echo{&count})});
        auto transport = scripted_transport{};
        auto observer = agent_observer{};
        auto request = nxtai::responses::openai_responses_request{.input = "inspect"};
        co_await nxtai::run_agent(request, tools, transport, observer);
        expect(count == 1);
        expect(observer.started == 2_ul && observer.finished == 2_ul);
        expect(observer.text_output == "answer");
        expect(transport.requests.size() == 2_ul);
        auto & next = transport.requests.at(1);
        expect(next.previous_response_id.empty());
        expect(next.input.empty());
        expect(next.input_items.size() == 6_ul);
        expect(next.input_items.at(0).str.find("inspect") != std::string::npos);
        expect(next.input_items.at(1).str.find("opaque-state") != std::string::npos);
        expect(next.input_items.at(2).str.find("call_1") != std::string::npos);
        expect(next.input_items.at(3).str.find("call_2") != std::string::npos);
        expect(next.input_items.at(4).str.find("first") != std::string::npos);
        expect(next.input_items.at(5).str.find("unknown tool") != std::string::npos);
        expect(next.tools.size() == 1_ul);
        expect(next.include == std::vector<std::string>{"reasoning.encrypted_content"});
    };
    "stored continuation sends only outputs with the response id"_test = []() -> nxtrt::task<void> {
        auto count = 0;
        auto tools = nxtai::tools::make_tool_registry({
            nxtai::tools::make_function_tool(counting_echo{&count})});
        auto transport = scripted_transport{};
        auto observer = agent_observer{};
        auto request = nxtai::responses::openai_responses_request{.input = "inspect", .store = true};
        co_await nxtai::run_agent(request, tools, transport, observer);
        auto & next = transport.requests.at(1);
        expect(next.previous_response_id == "resp_1");
        expect(next.input_items.size() == 2_ul);
        expect(next.tools.size() == 1_ul);
    };
    "caller tools and includes survive every agent turn without duplicates"_test =
        []() -> nxtrt::task<void> {
        auto count = 0;
        auto tools = nxtai::tools::make_tool_registry(
            {nxtai::tools::make_function_tool(counting_echo{&count})});
        auto transport = scripted_transport{};
        auto observer = agent_observer{};
        auto request =
            nxtai::responses::openai_responses_request{.input = "inspect"};
        request.tools = {
            {.name = "extra",
             .description = "caller extra",
             .parameters = raw_json{"{}"}},
            {.name = "echo",
             .description = "caller echo",
             .parameters = raw_json{"{}"}}};
        request.include = {
            "message.output_text.logprobs", "reasoning.encrypted_content"};
        co_await nxtai::run_agent(request, tools, transport, observer);
        expect(transport.requests.size() == 2_ul);
        for (const auto & sent : transport.requests) {
            expect(sent.tools.size() == 2_ul);
            expect(sent.tools.at(0).name == "extra");
            expect(sent.tools.at(1).description == "caller echo");
            expect(sent.include == request.include);
        }
        expect(request.model == "gpt-6-luna");
        expect(request.reasoning_effort.empty());
    };
    "turn limit prevents unanswerable extra tool work"_test = []() -> nxtrt::task<void> {
        auto count = 0;
        auto tools = nxtai::tools::make_tool_registry({
            nxtai::tools::make_function_tool(counting_echo{&count})});
        auto transport = scripted_transport{.repeat_calls = true};
        auto observer = agent_observer{};
        auto failed = false;
        try {
            co_await nxtai::run_agent({}, tools, transport, observer, {.max_turns = 2});
        } catch (const nxtrt::runtime_error &) { failed = true; }
        expect(failed);
        expect(count == 1);
        expect(transport.requests.size() == 2_ul);
    };
    "truncated stream never executes partial calls"_test = []() -> nxtrt::task<void> {
        auto count = 0;
        auto tools = nxtai::tools::make_tool_registry({
            nxtai::tools::make_function_tool(counting_echo{&count})});
        auto transport = scripted_transport{.truncate = true};
        auto observer = agent_observer{};
        auto failed = false;
        try { co_await nxtai::run_agent({}, tools, transport, observer); }
        catch (const nxtrt::runtime_error & e) {
            failed = std::string{e.what()}.find("ended before completion") != std::string::npos;
        }
        expect(failed);
        expect(count == 0);
    };
    "failed incomplete and error events fail the turn"_test = []() -> nxtrt::task<void> {
        for (auto type : {"response.failed", "response.incomplete", "error"}) {
            auto decoder = nxtai::responses::stream_decoder{};
            auto data = std::string{"{\"type\":\""} + type + "\",\"message\":\"fixture failure\"}";
            auto failed = false;
            try { co_await decoder.accept(type, data); }
            catch (const nxtrt::runtime_error & e) {
                failed = std::string{e.what()}.find("fixture failure") != std::string::npos;
            }
            expect(failed);
        }
    };
    "malformed completion and mismatched events are rejected"_test = []() -> nxtrt::task<void> {
        for (auto data : {R"({"type":"other"})",
                          R"({"type":"response.completed","response":{"id":"r","status":"completed","output":[{},]}})",
                          R"({"type":"response.completed","response":{"id":"r","status":"incomplete","output":[]}})",
                          R"({"type":"response.completed","response":{"id":"r","status":"completed","output":[]})"}) {
            auto decoder = nxtai::responses::stream_decoder{};
            auto failed = false;
            try { co_await decoder.accept("response.completed", data); }
            catch (const nxtrt::runtime_error &) { failed = true; }
            expect(failed);
        }
    };
}};
} // namespace
} // namespace nxt::test
